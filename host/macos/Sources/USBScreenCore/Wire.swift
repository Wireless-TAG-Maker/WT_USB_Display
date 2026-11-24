import Foundation
import zlib

// Single wire specification: firmware/main/usb_screen_protocol.h.
// This codec uses byte access to avoid unaligned integer loads from USB data.
public enum Wire {
    public static let vid: UInt16 = 0xCAFE
    public static let pid: UInt16 = 0x4011
    public static let width: UInt16 = 800
    public static let height: UInt16 = 1280
    public static let maxJPEG = 768 * 1024
    public static let frameMagic: UInt32 = 0x44345057
    public static let ackMagic: UInt32 = 0x41345057
    public static let helloMagic: UInt32 = 0x48345057
    // HS endpoint max packet is 512 bytes. Firmware's vendor RX waits for a
    // short packet; a complete frame ending on a full packet needs a ZLP.
    public static func needsTerminatingZLP(packetBytes: Int) -> Bool {
        packetBytes > 0 && packetBytes % 512 == 0
    }

    public static func u16(_ data: Data, _ offset: Int) -> UInt16 {
        UInt16(data[data.startIndex + offset]) | UInt16(data[data.startIndex + offset + 1]) << 8
    }
    public static func u32(_ data: Data, _ offset: Int) -> UInt32 {
        UInt32(u16(data, offset)) | UInt32(u16(data, offset + 2)) << 16
    }
    private static func put16(_ value: UInt16, into data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
    }
    private static func put32(_ value: UInt32, into data: inout Data) {
        put16(UInt16(truncatingIfNeeded: value), into: &data)
        put16(UInt16(truncatingIfNeeded: value >> 16), into: &data)
    }
    public static func crc32(_ data: Data) -> UInt32 {
        data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
            // zlib includes IEEE CRC32 pre/post conditioning; empty input is 0.
            UInt32(truncatingIfNeeded: crc32_z(0, bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count))
        }
    }
    public static func frame(sequence: UInt32, jpeg: Data) -> Data {
        precondition(!jpeg.isEmpty && jpeg.count <= maxJPEG)
        var result = Data()
        result.reserveCapacity(32 + jpeg.count)
        put32(frameMagic, into: &result)
        put16(1, into: &result); put16(32, into: &result)
        put32(sequence, into: &result); put32(UInt32(jpeg.count), into: &result)
        put16(width, into: &result); put16(height, into: &result)
        put16(1, into: &result); put16(0, into: &result)
        put32(crc32(jpeg), into: &result); put32(0, into: &result)
        result.append(jpeg)
        return result
    }
}

public struct Hello {
    public let maxJPEG: Int
    public let formats: UInt32
    public let preferredRefreshMillihz: UInt32
    public let capabilities: UInt32
    public init(_ data: Data) throws {
        guard data.count == 32, Wire.u32(data, 0) == Wire.helloMagic,
              Wire.u16(data, 4) == 1, Wire.u16(data, 6) == 32,
              Wire.u16(data, 8) == Wire.width, Wire.u16(data, 10) == Wire.height else {
            throw WireError.invalidHello
        }
        maxJPEG = min(Int(Wire.u32(data, 12)), Wire.maxJPEG)
        formats = Wire.u32(data, 16)
        preferredRefreshMillihz = Wire.u32(data, 20)
        capabilities = Wire.u32(data, 24)
        guard maxJPEG > 0, formats & (1 << 1) != 0 else { throw WireError.invalidHello }
    }
}

public enum WireError: Error { case invalidHello }

public struct Acknowledgement {
    public let sequence: UInt32
    public let status: UInt32
    public let received: UInt32
    public let decoded: UInt32
    public let presented: UInt32
    public let decodeUS: UInt32
    public let presentUS: UInt32
    public let refreshEvents: UInt32
    public init?(_ data: Data) {
        guard data.count == 40, Wire.u32(data, 0) == Wire.ackMagic,
              Wire.u16(data, 4) == 1, Wire.u16(data, 6) == 40 else { return nil }
        sequence = Wire.u32(data, 8); status = Wire.u32(data, 12)
        received = Wire.u32(data, 16); decoded = Wire.u32(data, 20)
        presented = Wire.u32(data, 24); decodeUS = Wire.u32(data, 28)
        presentUS = Wire.u32(data, 32); refreshEvents = Wire.u32(data, 36)
    }
}

public struct ACKParser {
    private var pending = Data()
    public init() {}
    public mutating func append(_ bytes: Data) -> [Acknowledgement] {
        pending.append(bytes)
        var result: [Acknowledgement] = []
        while pending.count >= 4 {
            if Wire.u32(pending, 0) != Wire.ackMagic { pending.removeFirst(); continue }
            guard pending.count >= 40 else { break }
            if let ack = Acknowledgement(Data(pending.prefix(40))) {
                result.append(ack); pending.removeFirst(40)
            } else { pending.removeFirst() }
        }
        return result
    }
}

/// Ensure ImageIO produced an actual 8-bit baseline image with native pixels.
public func isNativeBaselineJPEG(_ data: Data) -> Bool {
    data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
        guard bytes.count > 4, bytes[0] == 0xFF, bytes[1] == 0xD8 else { return false }
        var offset = 2
        while offset + 3 < bytes.count {
            guard bytes[offset] == 0xFF else { return false }
            while offset < bytes.count && bytes[offset] == 0xFF { offset += 1 }
            guard offset + 2 < bytes.count else { return false }
            let marker = bytes[offset]; offset += 1
            if marker == 0xDA || marker == 0xD9 { return false }
            if marker == 0x01 || (0xD0...0xD7).contains(marker) { continue }
            let length = Int(bytes[offset]) << 8 | Int(bytes[offset + 1])
            guard length >= 2, offset + length <= bytes.count else { return false }
            if marker == 0xC0 {
                guard length >= 8 else { return false }
                let height = Int(bytes[offset + 3]) << 8 | Int(bytes[offset + 4])
                let width = Int(bytes[offset + 5]) << 8 | Int(bytes[offset + 6])
                return bytes[offset + 2] == 8 && width == 800 && height == 1280
            }
            if (0xC1...0xCF).contains(marker) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC {
                return false
            }
            offset += length
        }
        return false
    }
}
