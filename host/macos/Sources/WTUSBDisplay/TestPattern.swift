import AppKit

@MainActor final class TestPatternView: NSView {
    private var timer: Timer?
    private var counter: UInt64 = 0
    private let began = ProcessInfo.processInfo.systemUptime
    override var isOpaque: Bool { true }
    func start() {
        let timer = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.counter &+= 1; self.needsDisplay = true
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }
    func stop() { timer?.invalidate(); timer = nil }
    override func draw(_ dirtyRect: NSRect) {
        let time = ProcessInfo.processInfo.systemUptime - began
        let phase = CGFloat(time.truncatingRemainder(dividingBy: 4) / 4)
        // Every broad stripe moves; this is not a mostly static dirty-tile test.
        for index in 0..<16 {
            NSColor(calibratedHue: (CGFloat(index) / 16 + phase).truncatingRemainder(dividingBy: 1), saturation: 0.8, brightness: 0.8, alpha: 1).setFill()
            NSRect(x: CGFloat(index) * bounds.width / 16, y: 0, width: bounds.width / 16 + 1, height: bounds.height).fill()
        }
        let x = CGFloat((sin(time * 2.7) + 1) / 2) * (bounds.width - 180)
        let y = CGFloat((cos(time * 3.1) + 1) / 2) * (bounds.height - 180)
        NSColor.white.setFill(); NSRect(x: x, y: y, width: 180, height: 180).fill()
        NSColor.black.setFill(); NSRect(x: 0, y: bounds.height - 100, width: bounds.width, height: 100).fill()
        let label = String(format: "WT FULL MOTION    FRAME %08llu    t %.3fs", counter, time)
        label.draw(at: NSPoint(x: 24, y: bounds.height - 72), withAttributes: [
            .font: NSFont.monospacedSystemFont(ofSize: 30, weight: .bold), .foregroundColor: NSColor.white,
        ])
    }
}

@MainActor final class TestPatternWindow {
    private var window: NSWindow?
    private var view: TestPatternView?
    func show(displayID: CGDirectDisplayID) async throws {
        var screen: NSScreen?
        for _ in 0..<40 {
            screen = NSScreen.screens.first {
                ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == displayID
            }
            if screen != nil { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        guard let screen else { throw DisplayError.message("Virtual display has no NSScreen for the test window") }
        let window = NSWindow(contentRect: screen.frame, styleMask: [.borderless], backing: .buffered, defer: false, screen: screen)
        window.isReleasedWhenClosed = false
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        let view = TestPatternView(frame: NSRect(origin: .zero, size: screen.frame.size))
        window.contentView = view; window.setFrame(screen.frame, display: true)
        window.orderFrontRegardless(); view.start()
        self.window = window; self.view = view
        print("TEST_PATTERN full_screen=virtual-only requested_animation_hz=60 visible_counter=yes")
    }
    func close() { view?.stop(); window?.close(); view = nil; window = nil }
}
