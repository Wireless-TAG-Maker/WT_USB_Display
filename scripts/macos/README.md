# macOS 验证工具

先在 Mac 上构建 App。Swift 离线检查与构建命令见 [macOS 上位机说明](../../host/macos/README.md#克隆构建与离线检查)。

从仓库根目录运行启动检查。脚本通过 LaunchServices 连续启动三次 App，每次创建虚拟屏，核对 1280×800 模式并等待 App 退出。日志写入 `outputs/usb_display/`。

```sh
bash scripts/macos/check_launch.sh
```

要测量正在运行的 App，在 Mac 上把 `$app_pid` 和 `$windowserver_pid` 设为对应进程号。工具分别报告各进程在采样窗口内的 CPU 用量，100% 表示占用一核。可用 `--fps-log app=日志路径` 同时提取该窗口新增的 `STATS` 记录。

```sh
python3 scripts/macos/measure_cpu.py --pid "app=$app_pid" --pid "WindowServer=$windowserver_pid" \
    --seconds 20 --output outputs/usb_display/macos-cpu.json
```
