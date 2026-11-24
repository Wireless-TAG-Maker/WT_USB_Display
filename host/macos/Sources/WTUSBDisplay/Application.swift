import AppKit
import CoreGraphics
import Foundation
import USBScreenCore

@MainActor final class Application: NSObject, NSApplicationDelegate {
    private let options: Options
    private var virtual: VirtualScreen?
    private var pattern: TestPatternWindow?
    private var usb: USBTransport?
    private var pipeline: FramePipeline?
    private var capture: Capture?
    private var synthetic: SyntheticFrames?
    private var statisticsTimer: DispatchSourceTimer?
    private var durationTimer: DispatchSourceTimer?
    private var signals: [DispatchSourceSignal] = []
    private var startupTask: Task<Void, Never>?
    private var stopping = false
    private var terminationPending = false

    init(options: Options) { self.options = options }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu()
        let applicationItem = NSMenuItem()
        menu.addItem(applicationItem)
        let submenu = NSMenu()
        submenu.addItem(withTitle: "Quit WT USB Display", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        applicationItem.submenu = submenu; NSApp.mainMenu = menu
        for number in [SIGINT, SIGTERM] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.stop() }
            source.resume(); signals.append(source)
        }
        startupTask = Task { [weak self] in
            guard let self else { return }
            do { try await self.start() }
            catch { if !self.stopping { self.stop(error: error) } }
        }
    }

    private func start() async throws {
        if !options.virtualOnly {
            guard options.synthetic || CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess() else {
                throw DisplayError.message("Screen Recording permission is required. Grant this app access in System Settings, then reopen it.")
            }
            let usb = try USBTransport(); self.usb = usb
            print("USB vid=CAFE pid=4011 speed=\(usb.speedName) native=800x1280 max_jpeg=\(usb.hello.maxJPEG) formats=\(usb.hello.formats) capabilities=\(usb.hello.capabilities)")
        }
        try Task.checkCancellation()
        if !options.synthetic {
            let virtual = VirtualScreen()
            virtual.onTerminated = { [weak self] in
                guard let self, !self.stopping else { return }
                self.stop(error: DisplayError.message("Virtual display terminated"))
            }
            self.virtual = virtual; try await virtual.create()
            if options.testPattern {
                let pattern = TestPatternWindow(); self.pattern = pattern
                try await pattern.show(displayID: virtual.id)
            }
        }
        try Task.checkCancellation()
        if let usb {
            let failed: (Error) -> Void = { [weak self] error in self?.stop(error: error) }
            let pipeline = FramePipeline(usb: usb, quality: options.quality, failure: failed)
            self.pipeline = pipeline
            pipeline.startReader()
            if options.synthetic {
                let generated = SyntheticFrames(pipeline: pipeline, fps: options.fps, failure: failed)
                synthetic = generated; generated.start()
            } else {
                guard let virtual else { throw DisplayError.message("Missing virtual display") }
                let capture = Capture(pipeline: pipeline, failure: failed)
                self.capture = capture
                try await capture.start(displayID: virtual.id, fps: options.fps)
            }
            try Task.checkCancellation()
            let timer = DispatchSource.makeTimerSource(queue: .main)
            timer.schedule(deadline: .now() + 1, repeating: 1)
            timer.setEventHandler { [weak self] in
                guard let self, let pipeline = self.pipeline else { return }
                print(pipeline.statistics.line(speed: usb.speedName))
            }
            timer.resume(); statisticsTimer = timer
        } else {
            print("VIRTUAL_ONLY no_USB=yes no_capture=yes")
        }
        if let seconds = options.seconds {
            let timer = DispatchSource.makeTimerSource(queue: .main)
            timer.schedule(deadline: .now() + seconds)
            timer.setEventHandler { [weak self] in self?.stop() }
            timer.resume(); durationTimer = timer
        }
        print("READY quality=\(options.quality); move windows onto the extended display; SIGINT/TERM or Quit cleans up.")
    }

    func stop(error: Error? = nil) {
        guard !stopping else { return }
        stopping = true
        if let error { fputs("ERROR: \(error.localizedDescription)\n", stderr) }
        statisticsTimer?.cancel(); statisticsTimer = nil
        durationTimer?.cancel(); durationTimer = nil
        startupTask?.cancel()
        Task { [self] in
            await startupTask?.value
            await capture?.stop()
            capture = nil
            synthetic?.stop(); synthetic = nil
            pattern?.close(); pattern = nil
            let finish: () -> Void = { [self] in
                if let pipeline, let usb { print(pipeline.statistics.line(speed: usb.speedName)) }
                pipeline = nil
                usb?.close(); usb = nil
                virtual?.destroy(); virtual = nil
                print("STOPPED capture=off USB=released virtual_display=removed")
                // Allow WindowServer to process display removal before exit.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                    if self.terminationPending { NSApp.reply(toApplicationShouldTerminate: true) }
                    exit(error == nil ? 0 : 1)
                }
            }
            if let pipeline { pipeline.stop(completion: finish) }
            else { finish() }
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        terminationPending = true
        stop()
        return .terminateLater
    }
}
