# Examples

两个工程共用 `common/easy_uacx_demo.c` 和 `common/easy_uacx_validation.c`，通过唯一公共头文件 `easy_uacx.h` 使用组件。新驱动完成两板矩阵后，旧参考播放实现已移除；历史验收数据保留在本文。

## 接线与构建

| 工程 | DAC 连接 | 烧录/日志 |
|---|---|---|
| `s3` | FS Host：D− GPIO19 / D+ GPIO20，带 VBUS 供电 | UART0；本机实测 COM9（CH343） |
| `p4` | 默认内部 HS PHY 的 Host 口，带 VBUS 供电 | USB Serial/JTAG；历史实测 COM10 |

实际 USB 速度由设备枚举决定，P4 上 FS DAC 仍为 FS。当前仅开放一个默认 Host 端口，不包含可选第二个 P4 FS 根端口。

```sh
cd examples/s3
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

P4 改为 `examples/p4` 和 `esp32p4`。Windows 的本机 ESP-IDF 位于 `C:\Espressif` 时，从仓库根目录运行：

```powershell
./scripts/idf_playback.ps1 -Board s3 -Port COM9
./scripts/idf_playback.ps1 -Board p4
# 描述符能力的全位深矩阵（各档一小节）
./scripts/idf_playback.ps1 -Board s3 -Profile Matrix -Port COM9
# 已验证矩阵 + 30 分钟 PCM24/48k + 释放资源检查
./scripts/idf_playback.ps1 -Board s3 -Profile Soak -Port COM9
```

不传 `-Port` 时只编译。Matrix/Soak 的配置和产物分别在工程的 `build/matrix/`、`build/soak/`，不改已有 `sdkconfig`；对应默认配置位于 `common/sdkconfig.matrix.defaults`、`common/sdkconfig.soak.defaults`。已存在的 profile 配置可使用该构建目录的 menuconfig 调整。

关闭其他占用串口的监视器后，可采集日志：

```powershell
C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe scripts/capture_serial.py --port COM9 --seconds 120 --reset --output examples/s3/matrix_serial.log
# 矩阵后 30 分钟播放，留出启动和收尾时间
C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe scripts/capture_serial.py --port COM9 --seconds 1900 --reset --output examples/s3/soak_serial.log
# 严格检查 UART 记录；缺失或失败时返回非零退出码
python scripts/check_validation.py examples/s3/matrix_serial.log --emit-caps
python scripts/check_validation.py examples/s3/soak_serial.log --require-soak
```

在 Windows 上，可按输入设备名称录制双声道并同步重置、采集板上日志：

```powershell
python -m pip install sounddevice soundfile
python scripts/record_input.py --input 'Input 1/2' --seconds 105 --output tmp/p4-audio/matrix.wav --serial COM10 --serial-python C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe
```

默认使用输入接口当前采样率（本机 UR22C 为 44.1 kHz），不打开监听输出。WAV、串口日志和带开始时间/丢帧计数的 JSON 同名保存；录音丢帧会返回非零状态，不能据此判断播放质量。

## 当前测试流程

1. 运行 **39 项 Unity 测试**：原有私有算法、驱动表、真实 CX31993 描述符、转换/反馈/RANGE、状态权限、计数器回绕、3 次 USB Host 初始化/释放及堆恢复、实际写入拼帧/批量转换/超时/abort、状态回调内 deinit、回调队列溢出顺序与完整性、8 个任务并发提交 128 个请求。
2. 等待 DAC，输出 `conn_id`、VID:PID、FS/HS、驱动、各输入位深能力及音量范围。设置播放时钟、设备侧 SET/GET INTERFACE；专用 CX31993 使用 −10.5 dB 初始音量。
3. 跨任务检查：外部任务 write/close 推送流被拒绝，abort 唤醒拥有任务；拉取回调阻塞时外部任务仍能查询音量，close 在回调退出后完成。输出 `VALIDATION`。
4. 对 `info.pcm[0/1/2]` 的每个位深和速率，分别使用拉取/推送播放《小星星》。故意以 4093 字节非帧对齐块供数。拉取 EOF 排空；推送保留尾部播放时间再 close。
5. 输出 `RESULT`、`MATRIX`、`VERIFIED_RATE`、`SUMMARY`。随后按配置运行 `SOAK`，每 30 秒输出 `STATS`；首轮最后可执行生命周期/堆检查并重新初始化。

`RESULT PASS` 表示开流、供数、连接及终止原因符合预期，仍需确认实际声音和最终 `stream stopped` 的错误/欠载统计。推送测试尾部等待期间会发送静音，可能计入欠载；30 分钟拉取验收要求全过程 `errors=0 underruns=0`，EOF 补静音不计欠载。

`VERIFIED_RATE` 是登记能力表的候选，必须结合听音或录音检查和日志的实际 alt/subslot，不能直接把描述符能力写成已验证表。Matrix 配置关闭已验证过滤，Soak 配置开启过滤。CX31993 FS 表已登记本轮 PCM16/24 六档；HS 表登记 PCM16/24/32 各八档（8/16/32/44.1/48/96/192/384 kHz）。S3 PCM32 端点 MPS768 超出当前 FIFO600，正确地从能力中排除。

## 配置

| 配置 | 默认 / 用途 |
|---|---|
| `EXAMPLE_PLAYBACK_TEST` | y；关闭后仅运行 Unity 测试 |
| `EXAMPLE_PLAYBACK_BARS` | 4；Matrix/Soak profile 为 1 |
| `EXAMPLE_SOAK_SECONDS` | 0；Soak profile 为 1800 |
| `EXAMPLE_LIFECYCLE_TEST` | y；Matrix profile 为 n；不影响前置 Unity 生命周期测试 |
| `EUACX_USE_VERIFIED_CAPS` | y；Matrix 为 n，遍历描述符能力 |
| `EUACX_NUM_TRANSFERS` | 4；范围 2–8 |
| `ESP_MAIN_TASK_STACK_SIZE` | 16384；供板上测试和示例缓冲使用 |
| `USB_HOST_CONTROL_TRANSFER_MAX_SIZE` | 4096；多 alt 描述符 |
| `ESP_CONSOLE_USB_SERIAL_JTAG` | P4 主控制台 |
| `USB_HOST_HUBS_SUPPORTED` | S3 启用外部 Hub |
| `USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` | S3 周期 OUT FIFO，支持 CX31993 的较大 MPS |
| `USB_HOST_EXT_PORT_RESET_RECOVERY_DELAY_MS` | S3 为 100 ms |
| `USB_HOST_EXT_PORT_CUSTOM_POWER_ON_DELAY_MS` | S3 为 500 ms；启用自定义等待 |

其他任务、队列、缓冲和重试选项见 [组件文档](../components/easy-esp-uacx/README.md)。

## 新驱动验收记录（2026-10-05）

环境：ESP-IDF 5.5.1 / USB Host 1.4.1，1.0.0-rc.1。

| 项目 | 结果 |
|---|---|
| S3 / P4 编译 | 均通过 |
| 主机算法测试 | CX31993 启用/禁用，各 33 项通过 |
| S3 板上 Unity | 37 项通过，0 失败 |
| S3 CX31993 枚举 | FS 成功；真实 422 字节描述符已作为测试夹具 |
| S3 推送拥有任务/abort | 通过；被阻塞的 write 已被其他任务唤醒 |
| S3 拉取阻塞与延迟 close | 通过；等待 close 时另一任务音量请求耗时 0 ms |
| S3 新驱动矩阵/听音 | 修复后 PCM16/24 六档 × 推送/拉取共 24 项通过，拉取零错误/零欠载；用户确认启动爆音消失 |
| 30 分钟零错误/零欠载 | S3 约 13 分 42 秒零错误/零欠载，换接 P4 时中断；完整 30 分钟待完成 |
| Hub 复位和拔插后自动恢复 | 待完整验收 |
| P4 新驱动实机 | 39 项 Unity、跨任务检查通过；PCM16/24/32 各八档 × 推送/拉取共 48 项通过，所有拉取零错误/零欠载 |
| P4 杂音定位与录音复测 | UR22C input 1/2、44.1 kHz 双声道采集，无录音丢帧；逐字节生成音乐使 24/32 位 384 kHz 欠载，改为逐帧后持续宽带噪声消失 |
| 异步反馈 DAC 实机 | 待完成；当前 CX31993 FS 描述符无反馈端点 |

重复初始化测试修复了 USB 库 pending event flags 未消耗导致卸载失败的问题；修复后堆恢复和重新初始化均通过。快速生命周期用例会在枚举尚未完成时关闭根端口，SDK 可能输出枚举取消相关日志，最终测试应为 PASS 且无内存损失。

正式发布还需：两块板按能力完成矩阵并听音、播放中拔插验证 stopped/disconnected 顺序和 `conn_id` 增长、Hub 复位恢复、30 分钟拉取无错误/欠载、带设备和流的反复 deinit 无泄漏。未通过前不标记正式 1.0 完成。
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

## Linux 设备行为计划的 P4 回归（2026-10-06）

P4 v1.3 / COM10 烧录当前实现的 Matrix 配置，ESP-IDF 5.5.1 / USB Host 1.4.1，CX31993 `06cb:1594`，HS。Flash 哈希校验通过。

39 项板上 Unity、推送 owner/abort/volume 与阻塞拉取 close/volume 检查全部通过。音量和静音能力正常，范围 [−18944, 0]/128，无枚举读回失败；音量请求耗时 0 ms。关闭已验证能力过滤后，PCM16/24/32 × 8/16/32/44.1/48/96/192/384 kHz × 推送/拉取，共 48 次全部通过；全部 ISO 错误为 0，所有拉取欠载为 0。推送尾部有意静音等待会增加欠载计数。严格日志检查通过，用户在播放期间确认声音正常。

原始日志保存在本机忽略的 `tmp/linux-p4-runtime.log`，未进行物理热插拔或 30 分钟持续播放。新特例标志通过主机模拟测试，本次硬件仅验证 flags=0 的既有 CX31993 默认行为；S3 回归及新 DAC 验收仍待完成。

## S3 当前实现回归（2026-10-06，未完成）

S3 v0.2 / CH343 COM9 烧录 Matrix 固件并通过 Flash 哈希校验。39 项板上测试和跨任务音量/abort/close 检查通过，CX31993 FS 音量/静音检测正常。已记录 PCM16 六档和 PCM24 前五档推送/拉取共 22 次 PASS，ISO 错误为 0，已完成拉取欠载为 0。

PCM24/96 kHz 开始时串口断开且 COM9 从系统消失，最后两次播放及汇总尚无结果，需重连 S3 烧录 USB 后补齐；听感尚未确认。日志保存在本机忽略的 `tmp/linux-s3-runtime.log` 和 `tmp/linux-s3-capture.log`。不计为完整矩阵通过。
