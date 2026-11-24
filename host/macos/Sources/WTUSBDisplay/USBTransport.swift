import Foundation
import Clibusb
import USBScreenCore

enum DisplayError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

final class USBTransport {
    private var context: OpaquePointer?
    private var handle: OpaquePointer?
    let hello: Hello
    let speedName: String

    init() throws {
        var context: OpaquePointer?
        let initial = libusb_init(&context)
        guard initial == 0 else { throw Self.error("libusb_init", initial) }
        self.context = context
        guard let handle = libusb_open_device_with_vid_pid(context, Wire.vid, Wire.pid) else {
            libusb_exit(context); self.context = nil
            throw DisplayError.message("USB CAFE:4011 not found or not accessible; connect the P4 high-speed OTG port.")
        }
        self.handle = handle
        let speed = libusb_get_device_speed(libusb_get_device(handle))
        speedName = speed >= LIBUSB_SPEED_HIGH.rawValue ? "high-speed" : "full/low-speed"
        guard speed >= LIBUSB_SPEED_HIGH.rawValue else {
            libusb_close(handle); libusb_exit(context); self.handle = nil; self.context = nil
            throw DisplayError.message("USB high-speed required; device is only \(speedName). Check the OTG port/cable.")
        }
        let claim = libusb_claim_interface(handle, 0)
        guard claim == 0 else {
            libusb_close(handle); libusb_exit(context); self.handle = nil; self.context = nil
            throw Self.error("claim interface 0", claim)
        }
        var bytes = [UInt8](repeating: 0, count: 32)
        let count = libusb_control_transfer(handle, 0xC1, 0x01, 0, 0, &bytes, 32, 1500)
        do {
            guard count == 32 else { throw Self.error("HELLO (expected 32 bytes)", Int32(count)) }
            hello = try Hello(Data(bytes))
        } catch {
            libusb_release_interface(handle, 0); libusb_close(handle); libusb_exit(context)
            self.handle = nil; self.context = nil
            throw error
        }
    }

    private static func error(_ operation: String, _ status: Int32) -> DisplayError {
        .message("\(operation): \(String(cString: libusb_error_name(status))) (\(status))")
    }

    // The writer and IN reader own concurrent transfers. close() is called
    // only after their DispatchGroup completes, never while libusb is active.
    func write(_ data: Data) throws {
        guard let handle else { throw DisplayError.message("USB closed") }
        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            var offset = 0
            while offset < raw.count {
                var transferred: Int32 = 0
                let count = min(raw.count - offset, 256 * 1024)
                let pointer = UnsafeMutablePointer(mutating: raw.baseAddress!.advanced(by: offset).assumingMemoryBound(to: UInt8.self))
                let status = libusb_bulk_transfer(handle, 0x01, pointer, Int32(count), &transferred, 1500)
                guard status == 0, transferred > 0 else { throw Self.error("bulk OUT", status) }
                offset += Int(transferred)
            }
        }
        if Wire.needsTerminatingZLP(packetBytes: data.count) {
            var transferred: Int32 = 0
            let status = libusb_bulk_transfer(handle, 0x01, nil, 0, &transferred, 1500)
            guard status == 0, transferred == 0 else { throw Self.error("bulk OUT terminating ZLP", status) }
        }
    }

    func readACKBytes() throws -> Data {
        guard let handle else { throw DisplayError.message("USB closed") }
        var data = [UInt8](repeating: 0, count: 4096)
        var count: Int32 = 0
        let status = libusb_bulk_transfer(handle, 0x81, &data, Int32(data.count), &count, 200)
        if status == LIBUSB_ERROR_TIMEOUT.rawValue { return Data(data.prefix(Int(count))) }
        guard status == 0 else { throw Self.error("bulk IN", status) }
        return Data(data.prefix(Int(count)))
    }

    func close() {
        if let handle {
            libusb_release_interface(handle, 0); libusb_close(handle); self.handle = nil
        }
        if let context { libusb_exit(context); self.context = nil }
    }
    deinit { close() }
}
