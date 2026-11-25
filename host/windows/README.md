# Windows USB 扩展桌面

Windows 端创建一个 16∶10 扩展屏，通过 DXGI 捕获这个屏幕。D3D11 将桌面旋转并缩放到面板的 800×1280 像素，主机按 JPEG 质量 72 编码，经 HUSB 投送到 P4。静止画面约每秒重发一次。

## 构建与启动

- P4 使用本分支的 `usb_display` 固件。HUSB 连接 Windows，设备应以 `CAFE:4011` 枚举为 USB High-Speed。固件通过 Microsoft OS 2.0 描述符请求绑定 WinUSB。固件构建和 FUSB 烧录步骤见[固件说明](../../firmware/README.md)。
- Windows 需要可交互的图形桌面、64 位 Python 3.10 或更高版本、64 位 `libusb-1.0.dll`、Visual Studio 2022 C++ 桌面工具、Windows SDK 和 .NET Framework 4.x 的 64 位 C# 编译器。
- 虚拟显示依赖已安装的 **Amyuni USB Mobile Monitor Virtual Display** 驱动，由 **Amyuni Technologies Inc.** 提供。工具使用该驱动的控制接口，按驱动自身许可获取和安装。

在仓库根目录的 PowerShell 中运行。将 DLL 路径换成本机实际位置。

```powershell
.\host\windows\build.cmd
$env:WT_LIBUSB_DLL = 'D:\tools\libusb-1.0.dll'
py -3 .\host\windows\usb_stream.py --list
py -3 .\host\windows\desktop.py start
```

枚举应显示一个设备、速度值为 3。启动成功会打印首个 P4 呈现回执和会话目录。可在 Windows 显示设置中移动扩展屏，再将应用窗口拖入该屏。每次启动创建一个独立会话，工具在已有 Amyuni 扩展屏时会退出。停止时使用启动时给出的命令。

```powershell
py -3 .\host\windows\desktop.py stop "<启动时输出的会话目录>"
```

默认构建目录为 `%LOCALAPPDATA%\WT_USB_Display\build`。指定其他目录时，将同一路径分别传给 `build.cmd <目录>` 和 `desktop.py start --build <目录>`。会话目录中的 `stream.json` 记录最近的呈现计数，`worker.log` 和 `capture.log` 记录传输与采集错误。

此前发布在 `scripts/windows/` 下的 `build.cmd`、`desktop.py` 和 `usb_stream.py` 命令也可使用。

实测桌面投送速度和实体屏更新率，见 [Windows 帧率验证](../../scripts/windows/README.md)。
