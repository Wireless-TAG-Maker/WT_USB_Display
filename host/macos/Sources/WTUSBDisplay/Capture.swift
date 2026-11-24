import Foundation
import CoreMedia
import CoreGraphics
import ScreenCaptureKit

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private let pipeline: FramePipeline
    private let failure: (Error) -> Void
    init(pipeline: FramePipeline, failure: @escaping (Error) -> Void) {
        self.pipeline = pipeline; self.failure = failure
    }
    func start(displayID: CGDirectDisplayID, fps: Int) async throws {
        var display: SCDisplay?
        for _ in 0..<40 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            display = content.displays.first { $0.displayID == displayID }
            if display != nil { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        // Deliberately never fall back to the main display.
        guard let display else { throw DisplayError.message("New virtual display never appeared in ScreenCaptureKit") }
        let configuration = SCStreamConfiguration()
        configuration.width = 1280; configuration.height = 800
        configuration.pixelFormat = kCVPixelFormatType_32BGRA
        configuration.minimumFrameInterval = CMTime(value: 1, timescale: Int32(fps))
        configuration.showsCursor = true
        configuration.queueDepth = 3
        let created = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: configuration, delegate: self)
        try created.addStreamOutput(self, type: .screen, sampleHandlerQueue: DispatchQueue(label: "wt.capture", qos: .userInitiated))
        try await created.startCapture()
        stream = created
        print("CAPTURE new_display_only=\(displayID) geometry=1280x800 fps_request=\(fps) output_JPEG=800x1280 rotation=counter-clockwise")
    }
    func stop() async { try? await stream?.stopCapture(); stream = nil }
    func stream(_ stream: SCStream, didOutputSampleBuffer buffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, buffer.isValid, let pixels = buffer.imageBuffer else { return }
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = attachments.first?[.status] as? Int,
              SCFrameStatus(rawValue: raw) == .complete else { return }
        pipeline.submit(pixels)
    }
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async { [failure] in failure(error) }
    }
}
