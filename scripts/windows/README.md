# Windows 扩展桌面验证

Windows 端创建一个 16∶10 扩展屏，通过 DXGI 捕获这个屏幕。D3D11 将桌面旋转并缩放到面板的 800×1280 像素，主机按 JPEG 质量 72 编码，经 HUSB 投送到 P4。静止画面约每秒重发一次。

## 构建与启动

- P4 使用本分支的 `usb_display` 固件。HUSB 连接 Windows，设备应以 `CAFE:4011` 枚举为 USB High-Speed。固件通过 Microsoft OS 2.0 描述符请求绑定 WinUSB。固件构建和 FUSB 烧录步骤见[固件说明](../../firmware/README.md)。
- Windows 需要可交互的图形桌面、64 位 Python 3.10 或更高版本、64 位 `libusb-1.0.dll`、Visual Studio 2022 C++ 桌面工具、Windows SDK 和 .NET Framework 4.x 的 64 位 C# 编译器。
- 虚拟显示依赖已安装的 **Amyuni USB Mobile Monitor Virtual Display** 驱动，由 **Amyuni Technologies Inc.** 提供。工具使用该驱动的控制接口，按驱动自身许可获取和安装。

在仓库根目录的 PowerShell 中运行。将 DLL 路径换成本机实际位置。

```powershell
.\scripts\windows\build.cmd
$env:WT_LIBUSB_DLL = 'D:\tools\libusb-1.0.dll'
py -3 .\scripts\windows\usb_stream.py --list
py -3 .\scripts\windows\desktop.py start
```

枚举应显示一个设备、速度值为 3。启动成功会打印首个 P4 呈现回执和会话目录。可在 Windows 显示设置中移动扩展屏，再将应用窗口拖入该屏。每次启动创建一个独立会话，工具在已有 Amyuni 扩展屏时会退出。停止时使用启动时给出的命令。

```powershell
py -3 .\scripts\windows\desktop.py stop "<启动时输出的会话目录>"
```

默认构建目录为 `%LOCALAPPDATA%\WT_USB_Display\build`。指定其他目录时，将同一路径分别传给 `build.cmd <目录>` 和 `desktop.py start --build <目录>`。会话目录中的 `stream.json` 记录最近的呈现计数，`worker.log` 和 `capture.log` 记录传输与采集错误。

## 通过 UVC 摄像头测帧率

保持扩展桌面运行。将 UVC 摄像头固定对准实体屏，让四角都在画面内，关闭占用摄像头的预览程序。HUSB 和摄像头由 Windows 使用，FUSB 串口交给 WSL 读取。WSL 的 Python 环境需要 NumPy 和带 `cv2.aruco.ArucoDetector`、`generateImageMarker` 的 OpenCV。

先在 PowerShell 中枚举摄像头的原生模式。以下为实测设备的 VID/PID，该设备的模式 13 是 1280×800、约 120 FPS 的 MJPG。其他设备需替换 VID/PID 并选择其实际 MJPG 模式。

```powershell
$env:WT_UVC_ID = '1bcf:2d4f'
& "$env:LOCALAPPDATA\WT_USB_Display\build\Camera.exe" list
```

在 WSL 的仓库根目录运行下列命令。第一个参数可用启动时打印的 Windows 会话路径或对应的 WSL 路径。第三、第四个参数可选，分别指定 Windows 构建目录、摄像头原生模式。更换摄像头时在 WSL 中也设置 `WT_UVC_ID`。

```bash
session='<启动时输出的会话目录>'
bash scripts/windows/fps/measure.sh "$session" /dev/ttyACM0
```

脚本先采样约 35 秒当前桌面，再在该扩展屏显示校准图、固定画面和 60 秒编号动画。摄像头采集与测试窗口共用 Windows 高精度时钟。分析器通过四角标记定位面板，独立解出两组编码，再用实际绘制记录核对每个编号及时间。

输出目录会打印在终端。`fps-result.json` 给出各阶段的 P4 呈现回执率、FUSB 计数和摄像头能读出的不同画面数。`fps-timeline.png` 保留逐秒波动。回执率表示固件提交速度，光学读数给出实体屏更新率下限。一次 1440×900 扩展桌面测试中，动态窗口平均收到 **57.52 次呈现回执/秒**，摄像头识别出 **2,385 个不同编号**，按首末有效样本计算为 **至少 39.73 次更新/秒**。

可用 `WT_FPS_PYTHON` 指定 WSL Python 解释器。成功分析后默认清理本次采集的原始 MJPEG 流，保留亮度样本、时间戳和逐帧编号。需要保留原始流时，在采集前设置 `WT_KEEP_UVC_RAW=1`。已有测量目录可离线复算。

```bash
python3 scripts/windows/fps/analyze_camera.py --replay "$result_dir"
python3 scripts/windows/fps/summarize.py "$result_dir"
```
