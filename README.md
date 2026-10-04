# easy-esp-uacx

ESP32-S3 / ESP32-P4 的 USB Audio Class 2.0 Host 项目，包含框架组件、音乐播放器和实机播放测试示例。

## 当前状态

`examples/s3` 与 `examples/p4` 已实现 USB DAC 枚举、采样率设置、音量/静音控制和立体声 PCM 播放。启动后先执行 20 项框架单元测试，再按 44.1、48、96、192 kHz 播放《小星星》四小节；设备不支持或端点带宽不足的档位会跳过。

2026-10-04 使用 CX31993 DAC（`06cb:1594`）完成实机验证，使用 24 位立体声 PCM，用户确认声音正常：

| 板型 | USB 速度 | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|---|
| ESP32-P4 v1.3 | HS | 通过 | 通过 | 通过 | 通过 |
| ESP32-S3 v0.2 | FS | 通过 | 通过 | 通过 | 跳过：端点带宽不足 |

两块板的 20 项单元测试均通过，已完成的播放档位均为零传输错误。详细接线、配置和测试数据见 [Examples 说明](examples/README.md)。

框架的能力选择、PCM/DoP/Native DSD 格式处理及 quirk 模块仍在开发中，部分设备管理、USB 适配和传输实现为占位代码。上述实机测试通过 `examples/common/uac2_playback_demo.c` 直接调用 ESP-IDF USB Host API，不能视为完整框架 API 或 DSD 实机验证。

## 目录

| 路径 | 用途 |
|---|---|
| `components/easy-esp-uacx/` | 自有框架组件、能力选择、格式处理与单元测试 |
| `examples/common/` | S3 / P4 共用的 USB DAC 播放测试程序 |
| `examples/s3/` | ESP32-S3 FS 测试工程 |
| `examples/p4/` | ESP32-P4 HS 测试工程 |
| `examples/player/` | ESP32-S3 音乐播放器，见 [播放器说明](examples/player/README.md) |
| `scripts/` | Windows 编译烧录和串口日志采集工具 |
| `third_party/esp-uac2-host/` | [Averyy/esp-uac2-host](https://github.com/Averyy/esp-uac2-host)，UAC2 参考驱动 |
| `third_party/esp-usb/` | [espressif/esp-usb](https://github.com/espressif/esp-usb)，USB 类驱动参考 |
| `third_party/micro-flac/` | [esphome-libs/micro-flac](https://github.com/esphome-libs/micro-flac)，FLAC 解码器 |

框架规划分层：App → Manager → Capability/Selector → PCM/DoP/DSD → Quirk → ISO engine → ESP USB Host。

## 克隆与快速开始

实机测试环境：ESP-IDF **5.5.1**，托管 USB Host 组件 **1.4.1**。

```sh
git clone --recurse-submodules https://github.com/easymcucourse/easy-esp-uacx.git
cd easy-esp-uacx
# 已克隆的仓库可补齐子模块：
git submodule update --init --recursive
```

在已加载 ESP-IDF 环境的终端中，选择一个测试工程执行：

```sh
cd examples/p4
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

S3 改用 `examples/s3` 和 `idf.py set-target esp32s3`。`PORT` 替换为实际串口。DAC 接到带 VBUS 供电的 USB Host 口；烧录/日志串口与 Host 口的用途不同。

## 测试范围与限制

- `SET_INTERFACE` 会在设备侧切换 alt0 / 播放 alt，并通过 `GET_INTERFACE` 验证；仅调用 `usb_host_interface_claim()` 不会启动 DAC 的播放端点。
- 播放前解除支持的静音控制，将支持的音量通道设为 −10.5 dB，并检查读回值；该设置来自 CX31993 S3 参考程序。
- 示例使用短时标称速率调度，未使用异步反馈，尚未验证长时间连续播放和时钟漂移。
- `RESULT` 的 PASS 表示 USB 传输完成；实际声音需听音确认。
- 实测 S3 Hub 连接在复位后出现过 Port2 枚举失败，重新插拔 DAC 后可完成测试；启动时的自动恢复尚未验证。
