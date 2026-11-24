import Foundation
import CoreImage
import CoreVideo
import ImageIO

public enum JPEGEncodingError: LocalizedError {
    case message(String)
    public var errorDescription: String? {
        if case .message(let text) = self { return text }; return nil
    }
}

/// Confined to the serial encoder queue. No capture, USB or permission APIs.
public final class NativeJPEGEncoder {
    private let context: CIContext
    private let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    private let quality: Double

    public init(quality: Double, softwareRenderer: Bool = false) {
        self.quality = quality
        context = CIContext(options: [.cacheIntermediates: false, .useSoftwareRenderer: softwareRenderer])
    }

    public func encode(_ pixels: CVPixelBuffer) throws -> Data {
        let jpeg: Data = try autoreleasepool {
            guard CVPixelBufferGetWidth(pixels) == 1280, CVPixelBufferGetHeight(pixels) == 800 else {
                throw JPEGEncodingError.message("Capture geometry must be 1280x800")
            }
            // The mounted WX101 panel needs counter-clockwise pixel rotation.
            // Clockwise was optically verified upside-down; EXIF alone is ignored by P4.
            let rotated = CIImage(cvPixelBuffer: pixels).oriented(.left)
            let image = rotated.transformed(by: CGAffineTransform(translationX: -rotated.extent.minX, y: -rotated.extent.minY))
            guard Int(image.extent.width) == 800, Int(image.extent.height) == 1280 else {
                throw JPEGEncodingError.message("Pixel rotation must produce 800x1280")
            }
            // Export the CI image directly. Materializing RGBA CGImage pixels
            // first forces the previous CPU ImageIO JPEG/alpha-conversion path.
            // On this Mac this keeps the original quality-0.72 quantization
            // tables while avoiding that readback/conversion step.
            let options: [CIImageRepresentationOption: Any] = [
                CIImageRepresentationOption(rawValue: kCGImageDestinationLossyCompressionQuality as String): quality,
            ]
            guard let data = context.jpegRepresentation(of: image, colorSpace: colorSpace, options: options) else {
                throw JPEGEncodingError.message("Core Image JPEG export failed")
            }
            return data
        }
        guard isNativeBaselineJPEG(jpeg) else {
            throw JPEGEncodingError.message("ImageIO output is not native 800x1280 SOF0 baseline JPEG")
        }
        return jpeg
    }
}
