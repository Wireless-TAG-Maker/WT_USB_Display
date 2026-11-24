import Foundation
import CoreVideo

/// USB diagnostic source only. No WindowServer, capture, TCC or user pixels.
final class SyntheticFrames {
    private let queue = DispatchQueue(label: "wt.synthetic", qos: .userInitiated)
    private var timer: DispatchSourceTimer?
    private let pipeline: FramePipeline
    private let fps: Int
    private var counter: UInt64 = 0
    private let failure: (Error) -> Void

    init(pipeline: FramePipeline, fps: Int, failure: @escaping (Error) -> Void) {
        self.pipeline = pipeline; self.fps = fps; self.failure = failure
    }
    func start() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / fps), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.generate() }
        self.timer = timer; timer.resume()
        print("SYNTHETIC generated_pixels=1280x800 rotated_JPEG=800x1280 fps_request=\(fps) counter=yes capture=no virtual_display=no TCC=no")
    }
    func stop() { timer?.cancel(); timer = nil }

    private func generate() {
        var pixels: CVPixelBuffer?
        let status = CVPixelBufferCreate(kCFAllocatorDefault, 1280, 800, kCVPixelFormatType_32BGRA, nil, &pixels)
        guard status == kCVReturnSuccess, let pixels else {
            DispatchQueue.main.async { [failure] in failure(DisplayError.message("Synthetic pixel allocation failed (\(status))")) }
            return
        }
        counter &+= 1
        CVPixelBufferLockBaseAddress(pixels, [])
        let base = CVPixelBufferGetBaseAddress(pixels)!
        let stride = CVPixelBufferGetBytesPerRow(pixels)
        // All broad stripes change every generated frame. Copying a row keeps
        // pixel generation cheap enough to diagnose decode/transport throughput.
        let phase = Int(counter % 256)
        var row = [UInt32](repeating: 0, count: 1280)
        for x in 0..<1280 {
            let r = UInt32((x / 5 + phase * 3) % 256)
            let g = UInt32((x / 9 + phase * 5) % 256)
            let b = UInt32((x / 3 + phase * 7) % 256)
            row[x] = 0xFF000000 | r << 16 | g << 8 | b
        }
        row.withUnsafeBytes { source in
            for y in 0..<800 { memcpy(base.advanced(by: y * stride), source.baseAddress!, 1280 * 4) }
        }
        func rect(_ x: Int, _ y: Int, _ width: Int, _ height: Int, _ color: UInt32) {
            for yy in max(0, y)..<min(800, y + height) {
                let destination = base.advanced(by: yy * stride).assumingMemoryBound(to: UInt32.self)
                for xx in max(0, x)..<min(1280, x + width) { destination[xx] = color }
            }
        }
        // A moving square and a binary frame counter visibly distinguish frames.
        rect(Int(counter * 11 % 1120), Int(counter * 7 % 600) + 100, 160, 100, 0xFFFFFFFF)
        rect(0, 0, 1280, 80, 0xFF101010)
        for bit in 0..<32 {
            let on = counter & (1 << bit) != 0
            rect(20 + bit * 38, 16, 28, 48, on ? 0xFFFFFFFF : 0xFF404040)
        }
        CVPixelBufferUnlockBaseAddress(pixels, [])
        pipeline.submit(pixels)
    }
}
