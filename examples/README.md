# Examples

## 工程与接线

| 工程 | 用途 | DAC 连接 | 日志连接 |
|---|---|---|---|
| `p4` | 单元测试 + 四档 PCM 播放测试 | P4 HS USB Host 口，带 VBUS 供电 | USB Serial/JTAG；默认主控制台 |
| `s3` | 单元测试 + 四档 PCM 播放测试 | S3 FS Host，D− GPIO19 / D+ GPIO20，带 VBUS 供电 | 默认 UART0；实测使用 CH343 |
| `player` | S3 音乐播放器 | 见 [播放器说明](player/README.md) | 见播放器工程配置 |

P4 使用 peripheral 0 和内部 HS PHY，实际速度以枚举日志 `HIGH (HS)` / `FULL (FS)` 为准，FS DAC 不会被强制升级为 HS。

## 编译与烧录

在 ESP-IDF 环境终端中，从仓库根目录进入对应工程：

```sh
cd examples/p4
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

S3 使用 `examples/s3` 和目标 `esp32s3`。`PORT` 替换为实际串口；实测 P4 为 COM10，S3 为 COM9。

仓库附带的 PowerShell 脚本适配本机 `C:\Espressif` 下的 ESP-IDF 5.5.1 安装。从仓库根目录运行，不传 `-Port` 时仅编译：

```powershell
./scripts/idf_playback.ps1 -Board p4 -Port COM10
./scripts/idf_playback.ps1 -Board s3 -Port COM9
```

关闭其他占用串口的监视器后，可采集一次启动及播放日志：

```powershell
C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe scripts/capture_serial.py --port COM9 --seconds 40 --reset --output examples/s3/playback_serial.log
```

## 播放流程与配置

两个测试工程共用 `common/uac2_playback_demo.c`，直接使用 ESP-IDF USB Host API。框架中的设备管理和传输占位实现尚未接入本测试。

1. 执行 20 项框架单元测试，等待 USB UAC2 DAC，打印速度和可用立体声 PCM alternate settings。
2. 选择满足带宽的格式，优先使用不超过 24 位的最高位深。
3. 发送设备侧 `SET_INTERFACE(alt0)` 并读回确认；解析播放通路上的 Feature Unit，解除静音、设置 −10.5 dB 音量并读回。
4. 设置并读回 DAC 采样率，在主机侧 claim 播放接口，再发送设备侧 `SET_INTERFACE(播放 alt)` 并读回确认。
5. 每档播放《小星星》四小节，约 4.8 秒音乐加 0.1 秒静音。输出 `RESULT` 和最终 `SUMMARY`，结束后切回 alt0。

采样率依次为 44100、48000、96000、192000 Hz。时钟设置不支持的档位或无满足带宽的格式会跳过；控制初始化和传输错误会报告失败。拔插 DAC 可重新测试，也可复位板卡。

| 配置 | 默认值/用途 |
|---|---|
| `CONFIG_EXAMPLE_PLAYBACK_TEST` | `y`；在 menuconfig 中关闭后仅运行单元测试 |
| `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` | 两个工程均为 4096，支持多 alternate settings 的较大描述符 |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | P4 主控制台，便于通过烧录口采集日志 |
| `CONFIG_USB_HOST_HUBS_SUPPORTED` | S3 启用外部 Hub 支持 |
| `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` | S3 周期 OUT FIFO 配置；实测默认 balanced 仅支持 128 字节，而 CX31993 alt2 的 FS MPS 为 576 字节 |

PCM 码率 = 采样率 × 通道数 × subslot 字节数 × 8。短时测试未使用异步反馈，未验证持续播放的时钟漂移；PASS 表示 USB 传输完成，声音仍需听音确认。

## 实机测试结果（2026-10-04）

环境：ESP-IDF 5.5.1 / USB Host 1.4.1，CX31993 DAC `06cb:1594`。两块板均使用时钟源 9、接口 1、alt2、24 位立体声 packed PCM；Feature Unit 2 的主通道静音读回为 OFF，左右通道音量读回均为 −10.5 dB。

### P4：HS，8000 intervals/s

ESP32-P4 v1.3，COM10。20 项单元测试通过，用户确认声音正常。

| 采样率 | PCM 码率 | 完成包数 | 完成字节数 | 错误数 | 结果 |
|---|---|---|---|---|---|
| 44.1 kHz | 2.1168 Mbit/s | 39200 | 1296540 | 0 | PASS |
| 48 kHz | 2.304 Mbit/s | 39200 | 1411200 | 0 | PASS |
| 96 kHz | 4.608 Mbit/s | 39200 | 2822400 | 0 | PASS |
| 192 kHz | 9.216 Mbit/s | 39200 | 5644800 | 0 | PASS |

每档耗时 4900–4901 ms。汇总：`passed=4 skipped=0 failed=0`。

### S3：FS，1000 intervals/s

ESP32-S3 v0.2，COM9（CH343 UART）。20 项单元测试通过，用户确认声音正常。

| 采样率 | 完成包数 | 完成字节数 | 错误数 | 结果 |
|---|---|---|---|---|
| 44.1 kHz | 4904 | 1297596 | 0 | PASS |
| 48 kHz | 4904 | 1412352 | 0 | PASS |
| 96 kHz | 4904 | 2824704 | 0 | PASS |
| 192 kHz | — | — | — | SKIP：DAC 的 FS 端点带宽不足 |

每档耗时 4914–4922 ms。汇总：`passed=3 skipped=1 failed=0`。

实测 Hub 连接在复位后出现 Port2 / CHECK_ADDR 枚举失败，拔插 DAC 后成功枚举并完成三档播放。当前未验证启动时的自动 Hub 恢复。

本地串口记录为 `p4/playback_set_interface_serial.log` 和 `s3/playback_fifo_serial.log`；日志文件由 `.gitignore` 排除，不随仓库提交。

## 排查要点

| 现象 | 检查内容 |
|---|---|
| ISO 传输 PASS 但无声 | 必须发送设备侧 `SET_INTERFACE` 启用播放 alt；同时检查静音和左右音量读回 |
| 描述符过大、枚举失败 | 检查控制传输上限是否为 4096 |
| S3 只发现 Hub | 启用外部 Hub 支持；如 Port2 枚举失败，重新插拔 DAC |
| S3 claim alt2 失败，MPS 576 超过 128 | 使用 `PERIODIC_OUT` FIFO 配置 |
| S3 跳过 192 kHz | 当前 DAC 的 FS alternate settings 带宽不足，属于预期结果 |
