# Third-party notices

The Objective-C `CGVirtualDisplayShim` declarations and the virtual-display
creation approach derive from **glint**, copyright (c) 2026 Shubham Kumar Gupta,
MIT licensed. The complete license is preserved in `GLINT-LICENSE` and bundled
in the app's Resources. Upstream reference (fixed revision; vendoring is not
required to build this host):

https://github.com/shbmx/glint/tree/9fd645e376baae6820a53d92a3c21acc24acca26

This host uses the separately installed libusb dynamic library. libusb is
LGPL-2.1-or-later; its system package remains separately distributed. No kernel
extension, DisplayLink driver, or system-wide installer is included.

`CGVirtualDisplay` is an undocumented CoreGraphics API; compatibility must be
tested on each macOS release. Its use here does not disable SIP or edit TCC.
