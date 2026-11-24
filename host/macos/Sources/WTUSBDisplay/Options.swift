import Foundation

struct Options {
    var probe = false, virtualOnly = false, permissionCheck = false, testPattern = false, synthetic = false
    var seconds: Double?, fps = 60, quality = 0.72
    static let help = """
    WTUSBDisplay — 1280x800@60 macOS extended display -> rotated 800x1280 USB JPEG
      --probe                USB high-speed/HELLO check only; no display or capture
      --permission-check     report existing Screen Recording consent; no prompt
      --virtual-only         create the virtual display without USB or screen capture
      --test-pattern         animate a full-screen counter pattern on the new display
      --synthetic            generated moving pixels -> JPEG -> USB; no capture/TCC/display
      --seconds N            stop and clean up after N seconds
      --fps N                capture request, 1..60 (default 60; --fps60 accepted)
      --quality Q            JPEG quality, 0.1..1 (default 0.72)
      --help                 show this help
    Capturing needs native Screen Recording consent for this app. No kext/SIP/TCC edits.
    """
    init(_ args: [String]) throws {
        var index = 0
        func value(_ option: String) throws -> String {
            guard index + 1 < args.count else { throw DisplayError.message("Missing value for \(option)") }
            index += 1; return args[index]
        }
        while index < args.count {
            let arg = args[index]
            switch arg {
            case "--probe": probe = true
            case "--virtual-only": virtualOnly = true
            case "--permission-check": permissionCheck = true
            case "--test-pattern": testPattern = true
            case "--synthetic": synthetic = true
            case "--seconds":
                let text = try value(arg)
                guard let number = Double(text), number.isFinite, number > 0 else { throw DisplayError.message("Invalid --seconds") }
                seconds = number
            case "--fps":
                guard let number = Int(try value(arg)), (1...60).contains(number) else { throw DisplayError.message("--fps must be 1..60") }
                fps = number
            case "--quality":
                guard let number = Double(try value(arg)), number.isFinite, (0.1...1).contains(number) else { throw DisplayError.message("--quality must be 0.1..1") }
                quality = number
            case "--help", "-h": print(Self.help); exit(0)
            default:
                if arg.hasPrefix("--fps"), let number = Int(arg.dropFirst(5)), (1...60).contains(number) { fps = number }
                else { throw DisplayError.message("Unknown option: \(arg)") }
            }
            index += 1
        }
        guard [probe, virtualOnly, permissionCheck, synthetic].filter({ $0 }).count <= 1 else {
            throw DisplayError.message("Choose only one of --probe, --virtual-only, --permission-check, --synthetic")
        }
        guard !testPattern || (!probe && !permissionCheck && !synthetic) else { throw DisplayError.message("--test-pattern needs a virtual display") }
    }
}
