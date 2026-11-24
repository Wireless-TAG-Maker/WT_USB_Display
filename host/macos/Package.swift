// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "WTUSBDisplay",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "WTUSBDisplay", targets: ["WTUSBDisplay"])],
    targets: [
        .systemLibrary(name: "Clibusb", pkgConfig: "libusb-1.0"),
        .target(name: "CGVirtualDisplayShim", linkerSettings: [.linkedFramework("CoreGraphics")]),
        .target(name: "USBScreenCore"),
        .executableTarget(name: "WTUSBDisplay", dependencies: ["Clibusb", "CGVirtualDisplayShim", "USBScreenCore"]),
        // A pure executable check also works with Command Line Tools, whose
        // installation does not include XCTest. It never opens USB or a display.
        .executableTarget(name: "USBScreenChecks", dependencies: ["USBScreenCore"], path: "Checks/USBScreenChecks"),
    ]
)
