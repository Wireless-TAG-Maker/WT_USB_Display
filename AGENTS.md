# 开发协作指引

本仓库只维护ESP32-P4 USB副屏固件、macOS上位机及其构建/验证工具。默认使用中文沟通。

## 修改入口

- 固件、显示时序、JPEG、USB或触摸改动前，阅读 [firmware/README.md](firmware/README.md) 和对应模块；协议字段以 `firmware/main/usb_screen_protocol.h` 为唯一来源。
- Mac捕获、编码、虚拟屏或打包改动前，阅读 [host/macos/README.md](host/macos/README.md) 和对应Swift模块。
- 保留现有用户改动，只调整本次任务相关文件。源代码的版权、SPDX、固定上游出处和许可文件随实现保留。

## 构建与验证

- 固件固定ESP-IDF **6.0.2** / esp32p4。USB配置使用 `WT_SCREEN_PROFILE=usb_display bash scripts/build_firmware.sh`；普通ASCII路径也可按固件README直接运行IDF。提交 `dependencies.lock`，托管组件和构建目录由工具生成。
- Mac使用 `bash host/macos/build.sh --test` 构建并运行纯检查。它不需要USB或录屏权限；编译成功不代替实际USB/显示验证。
- Shell改动运行 `bash -n`；Python工具运行语法及相应离线输入检查。提交前执行 `git diff --check`，审查完整暂存文件列表。
- 编译和文档发布不包含自动烧录、复位、启动捕获或权限重置。设备操作按用户已授权的具体范围执行，明确区分构建、烧录校验与运行结果。

## 设备合同

- 目标是WT9932P4C61-TINY的P4；FUSB为烧录/串口，HUSB为High-Speed视频设备。C61不在当前固件操作范围。
- USB显示为800×1280原生像素、RGB888/BGR24、三缓冲。调整格式时同步检查stride、容量、缓存对齐、JPEG输出长度和扫描缓冲所有权；LVGL benchmark/touch是独立RGB565配置。
- JPEG只写非扫描缓冲，换帧确认后才回收旧帧。保持队列有界；主机整512字节传输需要ZLP，CRC为IEEE CRC32。
- 精确屏初始化表和DCS命令ACK关闭设置已有实板依据；调整时序、引脚或命令前核实目标模组。0x5A身份、未知背光/复位GPIO和触摸控制值不应凭猜测写入。

## 性能与权限

- CPU/FPS比较使用同源内容、相同分辨率/质量/启动方式，并报告CPU窗口与帧率窗口。100% CPU表示一核；App、WindowServer和辅助进程分开列出。
- 60Hz/60FPS是请求，present ACK是固件提交计数，均不等于独立光学帧率。启动覆盖、稳态覆盖和设备错误分别记录；保留低速窗口。
- Core Image API本身不证明JPEG全程走GPU；不支持的硬件属性查询记录为未知，不能当作false。JPEG仍有损，RGB888缓冲不代表无损传输。
- ad-hoc重编会影响录屏授权身份。正常使用通过 `open` 启动App验证；CLI preflight成功不替代正常App授权。只在用户授权范围内恢复本App登记，保留其他App权限及系统安全设置。

## 发布范围

`.gitignore`使用白名单：仅收源码、锁文件、工程说明、正式工具和必要文档图片。新增目录时显式更新规则并复核暂存清单。`outputs/`、`vendor/`、下载资料、原始录像、个人路径/凭据、构建产物、临时诊断程序及旧测试草稿留在本地。公开截图只保留必要界面，发布前检查账户、路径和其他个人信息。
