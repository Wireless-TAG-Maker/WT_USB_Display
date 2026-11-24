import Foundation
import CoreMedia
import CoreVideo
import USBScreenCore

final class Statistics {
    private let lock = NSLock()
    private var captured: UInt64 = 0, encoded: UInt64 = 0, sent: UInt64 = 0
    private var rawDrops: UInt64 = 0, jpegDrops: UInt64 = 0, invalid: UInt64 = 0
    private var bytes: UInt64 = 0, presented: UInt64 = 0
    private var lastACK: Acknowledgement?
    private var previous: [UInt64] = Array(repeating: 0, count: 9)
    private var lastTime = ProcessInfo.processInfo.systemUptime
    func capture() { lock.lock(); captured += 1; lock.unlock() }
    func encode() { lock.lock(); encoded += 1; lock.unlock() }
    func dropRaw() { lock.lock(); rawDrops += 1; lock.unlock() }
    func dropJPEG() { lock.lock(); jpegDrops += 1; lock.unlock() }
    func reject() { lock.lock(); invalid += 1; lock.unlock() }
    func didSend(bytes count: Int) { lock.lock(); sent += 1; bytes += UInt64(count); lock.unlock() }
    func acknowledge(_ ack: Acknowledgement) {
        lock.lock(); defer { lock.unlock() }
        if let prior = lastACK {
            // Firmware counters are uint32_t and intentionally wrap.
            presented += UInt64(ack.presented &- prior.presented)
        }
        lastACK = ack
    }
    func line(speed: String) -> String {
        lock.lock(); defer { lock.unlock() }
        let now = ProcessInfo.processInfo.systemUptime
        let dt = max(now - lastTime, 0.001)
        let current = [captured, encoded, sent, presented, rawDrops, jpegDrops, invalid, bytes, UInt64(lastACK?.status ?? 0)]
        let d = zip(current, previous).map { $0 &- $1 }
        previous = current; lastTime = now
        var line = String(format: "STATS interval=%.3fs capture_fps=%.1f encode_fps=%.1f sent_fps=%.1f present_ack_fps=%.1f drops_capture=%llu drops_usb=%llu rejected=%llu usb_MBps=%.2f usb=%@",
                          dt, Double(d[0])/dt, Double(d[1])/dt, Double(d[2])/dt, Double(d[3])/dt,
                          d[4], d[5], d[6], Double(d[7])/dt/1_000_000, speed)
        if let ack = lastACK {
            line += " ack_seq=\(ack.sequence) status=\(ack.status) rx_total=\(ack.received) decode_total=\(ack.decoded) present_total=\(ack.presented) decode_us=\(ack.decodeUS) present_us=\(ack.presentUS) refresh_total=\(ack.refreshEvents)"
        } else { line += " ack=waiting" }
        return line
    }
}

final class FramePipeline {
    let statistics = Statistics()
    private let usb: USBTransport
    private let jpegEncoder: NativeJPEGEncoder
    private let failure: (Error) -> Void
    private let lock = NSLock()
    private let group = DispatchGroup()
    private let encoder = DispatchQueue(label: "wt.jpeg.encode", qos: .userInitiated)
    private let writer = DispatchQueue(label: "wt.usb.writer", qos: .userInitiated)
    private let reader = DispatchQueue(label: "wt.usb.ack", qos: .userInitiated)
    private var stopped = false, encoding = false, writing = false
    private var pendingPixels: CVPixelBuffer?
    private var pendingJPEG: Data?
    private var sequence: UInt32 = 0

    init(usb: USBTransport, quality: Double, failure: @escaping (Error) -> Void) {
        self.usb = usb; self.jpegEncoder = NativeJPEGEncoder(quality: quality); self.failure = failure
    }

    func startReader() {
        group.enter()
        reader.async { [self] in
            defer { group.leave() }
            var parser = ACKParser()
            while !isStopped {
                do {
                    for ack in parser.append(try usb.readACKBytes()) { statistics.acknowledge(ack) }
                } catch { if !isStopped { fail(error) }; break }
            }
        }
    }

    private var isStopped: Bool {
        lock.lock(); defer { lock.unlock() }; return stopped
    }

    func submit(_ pixels: CVPixelBuffer) {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        statistics.capture()
        if pendingPixels != nil { statistics.dropRaw() }
        pendingPixels = pixels
        if !encoding {
            encoding = true; group.enter()
            encoder.async { [self] in encodeLoop() }
        }
    }

    private func encodeLoop() {
        defer { group.leave() }
        while true {
            lock.lock()
            guard !stopped, let pixels = pendingPixels else {
                pendingPixels = nil; encoding = false; lock.unlock(); return
            }
            pendingPixels = nil; lock.unlock()
            do {
                let jpeg = try jpegEncoder.encode(pixels)
                guard jpeg.count <= usb.hello.maxJPEG else {
                    statistics.reject()
                    continue
                }
                statistics.encode()
                queueJPEG(jpeg)
            } catch { fail(error); return }
        }
    }

    private func queueJPEG(_ jpeg: Data) {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        if pendingJPEG != nil { statistics.dropJPEG() }
        pendingJPEG = jpeg
        if !writing {
            writing = true; group.enter()
            writer.async { [self] in writeLoop() }
        }
    }

    private func writeLoop() {
        defer { group.leave() }
        while true {
            lock.lock()
            guard !stopped, let jpeg = pendingJPEG else {
                pendingJPEG = nil; writing = false; lock.unlock(); return
            }
            pendingJPEG = nil
            sequence &+= 1; let current = sequence
            lock.unlock()
            do {
                let packet = Wire.frame(sequence: current, jpeg: jpeg)
                try usb.write(packet)
                statistics.didSend(bytes: packet.count)
            } catch { fail(error); return }
        }
    }

    private func fail(_ error: Error) {
        lock.lock()
        let first = !stopped
        stopped = true; pendingPixels = nil; pendingJPEG = nil
        lock.unlock()
        if first { DispatchQueue.main.async { [failure] in failure(error) } }
    }

    func stop(completion: @escaping () -> Void) {
        lock.lock(); stopped = true; pendingPixels = nil; pendingJPEG = nil; lock.unlock()
        group.notify(queue: .main, execute: completion)
    }
}
