import AppKit
import CoreGraphics
import Foundation
import USBScreenCore

setbuf(stdout, nil)
setbuf(stderr, nil)
do {
    let options = try Options(Array(CommandLine.arguments.dropFirst()))
    if options.permissionCheck {
        let granted = CGPreflightScreenCaptureAccess()
        print("PERMISSION screen_recording=\(granted ? "granted" : "not-granted") requested=no SIP_or_TCC_changes=no")
        exit(granted ? 0 : 2)
    }
    if options.probe {
        let transport = try USBTransport()
        print("PROBE vid=CAFE pid=4011 usb=\(transport.speedName) native=800x1280 hello_bytes=32 max_jpeg=\(transport.hello.maxJPEG) formats=\(transport.hello.formats) refresh_millihz=\(transport.hello.preferredRefreshMillihz) capabilities=\(transport.hello.capabilities)")
        transport.close()
        exit(0)
    }
    // The executable's entry point runs on the main thread. Make that
    // isolation explicit to Swift 6 without moving AppKit's run loop.
    MainActor.assumeIsolated {
        let application = NSApplication.shared
        let delegate = Application(options: options)
        application.setActivationPolicy(.regular)
        application.delegate = delegate
        application.run()
    }
} catch {
    fputs("ERROR: \(error.localizedDescription)\n", stderr)
    exit(1)
}
