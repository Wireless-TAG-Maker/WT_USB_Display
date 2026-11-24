import AppKit
import CoreGraphics
import CGVirtualDisplayShim

// Adapted from glint@9fd645e376baae6820a53d92a3c21acc24acca26 (MIT).
// See GLINT-LICENSE and THIRD_PARTY_NOTICES.md for the original notice.
@MainActor final class VirtualScreen {
    private var display: CGVirtualDisplay?
    var onTerminated: (() -> Void)?
    var id: CGDirectDisplayID { display?.displayID ?? 0 }

    func create() async throws {
        let descriptor = CGVirtualDisplayDescriptor()
        descriptor.queue = .main
        descriptor.name = "WT P4 USB Display"
        descriptor.maxPixelsWide = 1280; descriptor.maxPixelsHigh = 800
        descriptor.sizeInMillimeters = CGSize(width: 340, height: 213)
        descriptor.vendorID = UInt32(0xCAFE); descriptor.productID = UInt32(0x4011)
        // Stable identity, distinct from glint's SPI display.
        descriptor.serialNum = 0x57545034
        descriptor.terminationHandler = { [weak self] _, _ in
            DispatchQueue.main.async { self?.onTerminated?() }
        }
        guard let created = CGVirtualDisplay(descriptor: descriptor) else {
            throw DisplayError.message("CGVirtualDisplay creation failed on this macOS build")
        }
        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = 0
        settings.modes = [CGVirtualDisplayMode(width: 1280, height: 800, refreshRate: 60)]
        guard created.apply(settings) else { throw DisplayError.message("CGVirtualDisplay rejected 1280x800@60") }
        // Retain the display while yielding the main run loop. apply() does
        // not establish that WindowServer has published an online display.
        display = created
        do {
            for attempt in 1...3 {
                do {
                    let ready = try await waitForState(displayID: created.displayID, timeout: 3) { $0.hasRequestedMode }
                    print("VIRTUAL_READY attempt=\(attempt) \(ready.description)")
                    let mainID = CGMainDisplayID()
                    let mainBounds = CGDisplayBounds(mainID)
                    guard mainID != created.displayID, CGDisplayIsOnline(mainID) != 0,
                          mainBounds.width > 0, mainBounds.height > 0 else {
                        throw DisplayError.message("Main display is not ready for an extended layout")
                    }
                    let x = Int32(mainBounds.maxX), y = Int32(mainBounds.minY)
                    try configure(displayID: created.displayID, x: x, y: y, attempt: attempt)
                    let actual = try await waitForState(displayID: created.displayID, timeout: 2) {
                        $0.hasRequestedMode && !$0.mirrored && $0.mirrorsDisplay == 0 &&
                        abs($0.bounds.minX - CGFloat(x)) < 0.5 && abs($0.bounds.minY - CGFloat(y)) < 0.5
                    }
                    print("VIRTUAL display_id=\(created.displayID) requested=1280x800@60 HiDPI=off arrangement=right-of-main session-only")
                    print("VIRTUAL_ACTUAL \(actual.description)")
                    return
                } catch is CancellationError {
                    throw CancellationError()
                } catch {
                    print("VIRTUAL_RECONFIG attempt=\(attempt) failed=\(error.localizedDescription)")
                    guard attempt < 3 else { throw error }
                    // The cancelled/completed transaction is gone before
                    // waiting; never hold a configuration token over await.
                    try await Task.sleep(nanoseconds: 200_000_000)
                }
            }
        } catch {
            destroy()
            throw error
        }
    }

    private func configure(displayID: CGDirectDisplayID, x: Int32, y: Int32, attempt: Int) throws {
        var configuration: CGDisplayConfigRef?
        let begin = CGBeginDisplayConfiguration(&configuration)
        print("VIRTUAL_CONFIG attempt=\(attempt) step=begin cg_error=\(begin.rawValue)")
        guard begin == .success, let configuration else {
            throw DisplayError.message("CGBeginDisplayConfiguration failed (CGError \(begin.rawValue))")
        }
        var completed = false
        defer {
            if !completed {
                let cancel = CGCancelDisplayConfiguration(configuration)
                print("VIRTUAL_CONFIG attempt=\(attempt) step=cancel cg_error=\(cancel.rawValue)")
            }
        }
        let extend = CGConfigureDisplayMirrorOfDisplay(configuration, displayID, kCGNullDirectDisplay)
        print("VIRTUAL_CONFIG attempt=\(attempt) step=mirror_off cg_error=\(extend.rawValue)")
        let origin = CGConfigureDisplayOrigin(configuration, displayID, x, y)
        print("VIRTUAL_CONFIG attempt=\(attempt) step=origin target=\(x),\(y) cg_error=\(origin.rawValue)")
        guard extend == .success, origin == .success else {
            throw DisplayError.message("Virtual layout rejected: mirror CGError \(extend.rawValue), origin CGError \(origin.rawValue)")
        }
        let commit = CGCompleteDisplayConfiguration(configuration, .forSession)
        completed = true // The token is invalid after Complete returns, even on failure.
        print("VIRTUAL_CONFIG attempt=\(attempt) step=commit cg_error=\(commit.rawValue)")
        guard commit == .success else {
            throw DisplayError.message("CGCompleteDisplayConfiguration failed (CGError \(commit.rawValue))")
        }
    }

    private struct State {
        let displayID: CGDirectDisplayID
        let online: Bool
        let active: Bool
        let mirrored: Bool
        let mirrorsDisplay: CGDirectDisplayID
        let bounds: CGRect
        let mode: CGDisplayMode?

        var hasRequestedMode: Bool {
            guard online, let mode else { return false }
            return mode.width == 1280 && mode.height == 800 && mode.pixelWidth == 1280 &&
                   mode.pixelHeight == 800 && abs(mode.refreshRate - 60) < 0.1
        }
        var description: String {
            let geometry = mode.map {
                "logical=\($0.width)x\($0.height) pixels=\($0.pixelWidth)x\($0.pixelHeight) refresh_hz=\(String(format: "%.3f", $0.refreshRate))"
            } ?? "mode=unavailable"
            return "display_id=\(displayID) online=\(online ? 1 : 0) active=\(active ? 1 : 0) \(geometry) mirrored=\(mirrored ? 1 : 0) mirrors_display=\(mirrorsDisplay) origin=\(Int(bounds.minX)),\(Int(bounds.minY))"
        }
    }

    private func waitForState(displayID: CGDirectDisplayID, timeout: Double, matching: (State) -> Bool) async throws -> State {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        var stableSamples = 0
        while true {
            try Task.checkCancellation()
            let state = State(displayID: displayID, online: CGDisplayIsOnline(displayID) != 0,
                              active: CGDisplayIsActive(displayID) != 0, mirrored: CGDisplayIsInMirrorSet(displayID) != 0,
                              mirrorsDisplay: CGDisplayMirrorsDisplay(displayID), bounds: CGDisplayBounds(displayID),
                              mode: CGDisplayCopyDisplayMode(displayID))
            stableSamples = matching(state) ? stableSamples + 1 : 0
            if stableSamples >= 2 { return state }
            guard ProcessInfo.processInfo.systemUptime < deadline else {
                throw DisplayError.message("Timed out waiting for virtual display state: \(state.description)")
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
    }

    func destroy() {
        onTerminated = nil
        display = nil
    }
}
