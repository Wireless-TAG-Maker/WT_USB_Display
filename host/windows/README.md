# Wireless-tag USB 副屏（Windows）

## 安装与使用

适用于 Windows 10 2004 及以上的 x64 电脑，包括 Windows 11 x64。副屏需已烧录本项目带 WinUSB 支持的 USB 显示固件。

1. 双击 **Wireless-tag-USB-Display-Setup.exe**，点击“安装”。安装包支持离线使用。首次添加虚拟显示驱动时，按 Windows 提示批准管理员权限。单位统一管理的电脑可能需要由 IT 批准这一步。
2. 应用会自动打开。用 USB 数据线将电脑接到板子的 **HUSB** 口。看到“副屏已连接”后，将窗口拖到扩展屏即可使用。“显示器排列”可调整两块屏幕的相对位置。
3. 需要开机使用时，在主窗口的“显示设置”中打开“登录 Windows 时自动启动”。之后登录当前账号，应用会在通知区域等待连接。拔线会停止投屏，接回后会重连。

点击窗口右上角的关闭按钮可收起到通知区域。双击托盘图标，或从开始菜单打开 **Wireless-tag USB 副屏**（英文安装时为 **Wireless-tag USB Display**），可再次进入设置。点击“暂停连接”可停止投屏和自动重连，之后点击一次“连接副屏”即会恢复投屏。选择 **更多 → 退出** 会结束投屏并退出应用。

点击“显示设置”右侧的 **更多**（或按 **F10**）可打开应用菜单。在 **更多 → 语言 / Language** 中选择 **简体中文** 或 **English**，界面会立即切换，当前连接继续工作。应用会记住当前 Windows 用户的选择，覆盖安装也会保留。打开 **更多 → 帮助 → 关于** 可查看版本和版权声明。点击公司名可打开相应语言的公司简介，窗口也提供 Wireless-tag 官网、中文文档和项目开源仓库的链接。选择 **更多 → 帮助 → 获取更新** 会在默认浏览器中打开 GitHub Releases，可在那里下载新版安装包。

在主窗口的 **显示设置 → 色度采样** 中可随时切换 **标准色度 (4:2:0)** 和 **完整色度 (4:4:4)**。默认的标准色度占用较少 USB 带宽。完整色度保留每个像素的色彩细节，适合查看彩色小字和细线，滚动时帧率可能降低。选择立即生效，应用下次启动时会沿用。两档使用相同的桌面分辨率和面板像素数。

可从 Windows 的“已安装的应用”卸载。虚拟显示驱动可能由其他软件共用，卸载程序会保留它。

打开应用或点击“连接副屏”时，如果 Windows 已有一块兼容的 Amyuni 虚拟扩展屏，应用会直接使用它投屏。否则，若尚无虚拟屏，应用会自动创建一块。暂停或拔线只会移除应用自己创建的屏幕，已有屏幕沿用原来的排列和分辨率。

部分笔记本首次连接时，Windows 会将新副屏设为复制，在“显示器排列”中显示为 **1|2**。应用会尝试将本次创建的副屏切为独立扩展屏。如果窗口提示“请启用扩展显示”，点击 **显示器排列**，选择 **扩展这些显示器** 并保留更改，应用会继续连接。

登录 Windows 后在通知区域自动启动时，如果首次遇到已有虚拟屏，窗口会显示它的编号和分辨率。点击一次“连接副屏”即可开始投送，本次会话里拔线再接会自动恢复到同一块屏幕。若有多块 Amyuni 虚拟屏，应用会提示只启用要投送的一块。

安装包包含 Python、libusb 和 Amyuni Technologies Inc. 的原版虚拟显示驱动。许可文件随程序安装，Amyuni 的许可提到可能展示广告页。[驱动原包与许可](https://www.amyuni.com/downloads/usbmmidd_v2.zip) · [第三方组件说明](THIRD_PARTY_NOTICES.txt)

## 为用户构建安装包

在 Windows x64 构建机上准备 Python 3.10+、Visual Studio 2022 C++ 桌面工具、Windows SDK、.NET Framework 4.x 的 64 位 C# 编译器和 Inno Setup 6（此版本用 6.4.3 构建）。在仓库根目录运行以下命令。

```powershell
py -3 .\host\windows\package.py
```

产物固定为 **outputs/windows-installer/Wireless-tag-USB-Display-Setup.exe**。将这一个文件交付给用户即可。打包工具从官方来源下载固定版本的运行时和驱动，校验 SHA-256 后缓存。可用 `--cache` 指定缓存目录，`--iscc` 指定编译器，`--output` 指定交付目录。

构建默认生成未签名的应用安装包。机构分发时可用 `--sign-command` 传入企业已有的代码签名命令。命令使用 Inno Setup 的 `$f` 文件占位符，PowerShell 中用单引号保留它。打包器将对自有程序、驱动安装程序、卸载程序和最终安装包签名。Amyuni 驱动保留原厂签名。[Inno Setup 签名配置](https://jrsoftware.org/ishelp/topic_setup_signtool.htm)

## 开发与命令行验证

Windows 端创建一个 16∶10 扩展屏，通过 DXGI 捕获这个屏幕。D3D11 将桌面旋转并缩放到面板的 800×1280 像素，主机按 JPEG 质量 72 编码，经 HUSB 投送到 P4。4:4:4 使用 Windows Imaging Component，若单帧超过设备协商的 JPEG 大小上限，仅降低该帧的 JPEG 质量来保留完整色度。静止画面约每秒重发一次。独立命令行工具默认使用 4:2:0。[微软 JPEG 编码参数说明](https://learn.microsoft.com/en-us/windows/win32/wic/jpeg-format-overview#encoder-options)

以下命令供开发者独立验证。开发环境另需 64 位 libusb DLL 和已安装的 Amyuni USB Mobile Monitor Virtual Display 驱动。HUSB 应以 `CAFE:4011` 枚举为 USB High-Speed。固件通过 Microsoft OS 2.0 描述符请求绑定 WinUSB。[固件构建与烧录说明](../../firmware/README.md)

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

默认构建目录为 `%LOCALAPPDATA%\WT_USB_Display\build`。指定其他目录时，将同一路径分别传给 `build.cmd <目录>` 和 `desktop.py start --build <目录>`。会话目录中的 `stream.json` 包含呈现计数、最近一帧呈现回执对应的 JPEG 色度采样 `ack_chroma` 和已确认的 JPEG 有效字节累计值 `ack_jpeg_bytes`。`worker.log` 和 `capture.log` 记录传输与采集错误。

此前发布在 `scripts/windows/` 下的 `build.cmd`、`desktop.py` 和 `usb_stream.py` 命令也可使用。

实测桌面投送速度和实体屏更新率，见 [Windows 帧率验证](../../scripts/windows/README.md)。
