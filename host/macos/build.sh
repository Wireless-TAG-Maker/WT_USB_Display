#!/bin/bash
# Build/sign a local app only. Never launch it or modify system permissions.
set -euo pipefail
WT_HOST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${WT_HOST_ROOT}"
pkg-config --exists libusb-1.0
mkdir -p .build/clang-module-cache .build/swift-module-cache .build/swiftpm-cache .build/swiftpm-config .build/swiftpm-security
export CLANG_MODULE_CACHE_PATH="${WT_HOST_ROOT}/.build/clang-module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="${WT_HOST_ROOT}/.build/swift-module-cache"
# SwiftPM's nested manifest sandbox cannot be entered from the Codex sandbox.
# This changes only that build subprocess, not SIP/TCC or the outer sandbox.
WT_HOST_SWIFT_FLAGS=(--disable-sandbox --cache-path .build/swiftpm-cache --config-path .build/swiftpm-config --security-path .build/swiftpm-security)
swift build "${WT_HOST_SWIFT_FLAGS[@]}" -c release
WT_HOST_BIN_DIR="$(swift build "${WT_HOST_SWIFT_FLAGS[@]}" -c release --show-bin-path)"
WT_HOST_APP="${WT_HOST_ROOT}/build/WTUSBDisplay.app"
mkdir -p "${WT_HOST_APP}/Contents/MacOS" "${WT_HOST_APP}/Contents/Resources"
cp "${WT_HOST_BIN_DIR}/WTUSBDisplay" "${WT_HOST_APP}/Contents/MacOS/WTUSBDisplay"
cp GLINT-LICENSE THIRD_PARTY_NOTICES.md "${WT_HOST_APP}/Contents/Resources/"
cat > "${WT_HOST_APP}/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.wirelesstag.WTUSBDisplay</string>
<key>CFBundleName</key><string>WT USB Display</string>
<key>CFBundleDisplayName</key><string>WT USB Display</string>
<key>CFBundleExecutable</key><string>WTUSBDisplay</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.2.0</string>
<key>CFBundleVersion</key><string>2</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSScreenCaptureUsageDescription</key><string>Capture only the new WT USB virtual display and send its pixels to the connected ESP32-P4 panel.</string>
</dict></plist>
PLIST
codesign --force --sign - "${WT_HOST_APP}"
codesign --verify --strict "${WT_HOST_APP}"
printf 'Built local app: %s\n' "${WT_HOST_APP}"
if [[ "${1:-}" == "--test" ]]; then
    swift run "${WT_HOST_SWIFT_FLAGS[@]}" -c release USBScreenChecks
fi
