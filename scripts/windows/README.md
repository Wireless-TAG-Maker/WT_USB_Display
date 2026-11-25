# Windows 扩展桌面验证

先编译 Windows 上位机和帧率工具，再启动扩展桌面。构建命令、依赖和会话目录见[上位机说明](../../host/windows/README.md)。

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
