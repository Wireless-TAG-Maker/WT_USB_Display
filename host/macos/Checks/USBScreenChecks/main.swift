import Foundation
import CoreVideo
import ImageIO
import USBScreenCore

enum CheckFailure: Error { case failed(String) }
func expect(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    guard condition() else { throw CheckFailure.failed(message) }
}
func mustThrow(_ body: () throws -> Void, _ message: String) throws {
    do { try body() } catch { return }
    throw CheckFailure.failed(message)
}

func checkWire() throws {
    let payload = Data("123456789".utf8)
    try expect(Wire.crc32(payload) == 0xCBF43926, "IEEE CRC32 known vector")
    let packet = Wire.frame(sequence: 0x12345678, jpeg: payload)
    try expect(packet.count == 41, "header plus payload size")
    try expect(Array(packet.prefix(8)) == [0x57, 0x50, 0x34, 0x44, 1, 0, 32, 0], "WP4D/version/header bytes")
    try expect(Wire.u32(packet, 8) == 0x12345678 && Wire.u32(packet, 12) == 9, "sequence/payload length")
    try expect(Wire.u16(packet, 16) == 800 && Wire.u16(packet, 18) == 1280, "native geometry")
    try expect(Wire.u16(packet, 20) == 1 && Wire.u16(packet, 22) == 0, "JPEG format/flags")
    try expect(Wire.u32(packet, 24) == 0xCBF43926 && Wire.u32(packet, 28) == 0, "CRC/reserved")
    try expect(packet.suffix(9) == payload, "unaltered payload")
    try expect(Wire.needsTerminatingZLP(packetBytes: 512), "512-byte frame needs ZLP")
    try expect(Wire.needsTerminatingZLP(packetBytes: 8192), "8192-byte frame needs ZLP")
    try expect(!Wire.needsTerminatingZLP(packetBytes: 511) && !Wire.needsTerminatingZLP(packetBytes: 513), "short tail already terminates frame")
    try expect(!Wire.needsTerminatingZLP(packetBytes: 0), "empty data is not a frame")
    try expect(Wire.crc32(Data()) == 0, "IEEE CRC32 empty input")
    // Independent bitwise reference catches CRC32C/seed/final-XOR mistakes
    // when switching the production implementation to the system library.
    var random: UInt32 = 0x57445034
    for length in [1, 15, 511, 512, 8192, 208 * 1024, Wire.maxJPEG] {
        var bytes = [UInt8](repeating: 0, count: length)
        for i in bytes.indices {
            random = random &* 1664525 &+ 1013904223
            bytes[i] = UInt8(truncatingIfNeeded: random >> 24)
        }
        var expected: UInt32 = 0xFFFFFFFF
        for byte in bytes {
            expected ^= UInt32(byte)
            for _ in 0..<8 {
                expected = (expected >> 1) ^ ((expected & 1) != 0 ? 0xEDB88320 : 0)
            }
        }
        expected ^= 0xFFFFFFFF
        let data = Data(bytes)
        try expect(Wire.crc32(data) == expected, "IEEE CRC differential length \(length)")
        let packet = Wire.frame(sequence: UInt32(length), jpeg: data)
        try expect(packet.count == length + 32 && Wire.u32(packet, 24) == expected &&
                   packet.suffix(length) == data, "CRC packet/payload length \(length)")
    }
}

func checkACK() throws {
    let ack = Data([0x57, 0x50, 0x34, 0x41, 1, 0, 40, 0] +
                   [7, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0,
                    6, 0, 0, 0, 25, 0, 0, 0, 33, 0, 0, 0, 99, 0, 0, 0])
    var parser = ACKParser()
    try expect(parser.append(Data([0, 1, 2]) + ack.prefix(13)).isEmpty, "fragment must wait")
    let parsed = parser.append(ack.dropFirst(13) + ack)
    try expect(parsed.count == 2, "fragmentation/coalescing/noise resync")
    try expect(parsed[0].sequence == 7 && parsed[0].status == 0, "ACK sequence/status")
    try expect(parsed[0].received == 9 && parsed[0].decoded == 8 && parsed[0].presented == 6, "cumulative counters")
    try expect(parsed[0].decodeUS == 25 && parsed[0].presentUS == 33 && parsed[0].refreshEvents == 99, "ACK timings/refresh")
}

func checkHelloAndGuard() throws {
    let hello = Data([0x57, 0x50, 0x34, 0x48, 1, 0, 32, 0, 0x20, 3, 0, 5,
                      0, 0, 12, 0, 2, 0, 0, 0, 0x60, 0xEA, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0])
    let parsed = try Hello(hello)
    try expect(parsed.maxJPEG == 768 * 1024 && parsed.formats == 2, "HELLO max/format bitmask")
    try expect(parsed.preferredRefreshMillihz == 60000 && parsed.capabilities == 3, "HELLO refresh/capabilities")
    try mustThrow({ _ = try Hello(Data(hello.dropLast())) }, "truncated HELLO accepted")
    var wrongFormat = hello; wrongFormat[16] = 1
    try mustThrow({ _ = try Hello(wrongFormat) }, "JPEG enum mistaken for bitmask")
    let sof = Data([0xFF, 0xD8, 0xFF, 0xE0, 0, 2, 0xFF, 0xC0, 0, 8, 8, 5, 0, 3, 0x20, 3])
    try expect(isNativeBaselineJPEG(sof), "SOF0 native guard")
    var progressive = sof; progressive[7] = 0xC2
    try expect(!isNativeBaselineJPEG(progressive), "progressive JPEG accepted")
    var landscape = sof; landscape[11] = 3; landscape[12] = 0x20; landscape[13] = 5; landscape[14] = 0
    try expect(!isNativeBaselineJPEG(landscape), "EXIF-only landscape JPEG accepted")
}

func pixelBuffer(width: Int, height: Int) throws -> CVPixelBuffer {
    var pixels: CVPixelBuffer?
    let status = CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, nil, &pixels)
    guard status == kCVReturnSuccess, let pixels else { throw CheckFailure.failed("synthetic pixel buffer allocation") }
    CVPixelBufferLockBaseAddress(pixels, [])
    defer { CVPixelBufferUnlockBaseAddress(pixels, []) }
    let base = CVPixelBufferGetBaseAddress(pixels)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(pixels)
    for y in 0..<height {
        for x in 0..<width {
            let offset = y * stride + x * 4
            base[offset] = UInt8((x * 255) / width)
            base[offset + 1] = UInt8((y * 255) / height)
            base[offset + 2] = 220
            base[offset + 3] = 255
        }
    }
    return pixels
}

func checkRealJPEG() throws {
    let encoder = NativeJPEGEncoder(quality: 0.72, softwareRenderer: true)
    let jpeg = try encoder.encode(pixelBuffer(width: 1280, height: 800))
    try expect(!jpeg.isEmpty && jpeg.count < Wire.maxJPEG, "synthetic JPEG byte limit")
    try expect(isNativeBaselineJPEG(jpeg), "real ImageIO output must be native SOF0")
    guard let source = CGImageSourceCreateWithData(jpeg as CFData, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
          let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any] else {
        throw CheckFailure.failed("ImageIO could not decode generated JPEG")
    }
    try expect(image.width == 800 && image.height == 1280, "decoded real pixel dimensions")
    try expect((props[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1 == 1, "EXIF orientation must be normal")
    // On this mounted panel the source top-right must become the native
    // top-left. A dimensions-only check also passes for the upside-down image.
    var rgba = [UInt8](repeating: 0, count: image.width * image.height * 4)
    try rgba.withUnsafeMutableBytes { raw in
        guard let context = CGContext(data: raw.baseAddress, width: image.width, height: image.height,
                                      bitsPerComponent: 8, bytesPerRow: image.width * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue) else {
            throw CheckFailure.failed("JPEG corner sample context")
        }
        context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        let offset = (40 * image.width + 40) * 4
        let samples = raw.bindMemory(to: UInt8.self)
        try expect(samples[offset + 1] < 35 && samples[offset + 2] > 220,
                   "mounted-panel rotation: native top-left G=\(samples[offset + 1]) B=\(samples[offset + 2]) must come from source top-right")
    }
    try mustThrow({ _ = try encoder.encode(pixelBuffer(width: 800, height: 1280)) }, "unexpected input dimensions accepted")
    print("CHECK JPEG generated_bytes=\(jpeg.count) decoded=800x1280 SOF0=yes orientation=1")
}

do {
    try checkWire(); print("PASS wire_crc_and_layout")
    try checkACK(); print("PASS ack_fragmentation_and_resync")
    try checkHelloAndGuard(); print("PASS hello_and_baseline_guard")
    try checkRealJPEG(); print("PASS synthetic_pixel_rotation_and_real_JPEG")
    print("CHECKS PASS 4/4; capture=no USB=no virtual_display=no permissions=no")
} catch {
    fputs("CHECKS FAIL: \(error)\n", stderr)
    exit(1)
}
