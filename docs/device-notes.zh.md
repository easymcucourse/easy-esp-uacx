<!-- SPDX-FileCopyrightText: 2026 easymcucourse -->
<!-- SPDX-License-Identifier: MIT -->
# USB DAC 设备行为记录

[English](device-notes.md) | 中文

本文件仅记录 USB ID、现象及条件、出处和本项目证据，不记录外部驱动的实现。协议依据 USB-IF Audio 2.0 第 4.9.2、5.2.6.1.2、A.17.11 节；同格式最小 MPS 行为也见 [微软 USB Audio 2.0 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)。UAC1 实机记录也列于此处。驱动条目按实际 USB ID 登记。

## 06cb:1594 CX31993
- 现象：FS/HS 的 alt 与 Host FIFO 限制不同；音量使用左右可写通道，静音使用 master。
- 来源：[本项目验收记录](../plan.zh.md)、[真实 FS 描述符](../components/easy-esp-uacx/test/fixtures/cx31993_fs.h)。
- 证据等级：verified（现有条目）。
- 本项目条目：CX31993，flags = 0，保持既有 −10.5 dB 初始音量及 FS/HS 能力表。
- 本次回归：2026-10-06 P4 v1.3 / COM10 / HS，39 项板上测试和 PCM16/24/32 各八档推送/拉取共 48 次通过；全部 ISO 错误为 0、拉取欠载为 0，音量和静音读回检测通过，用户确认声音正常。S3 新控制逻辑回归仍待完成。

## 262a:0002 ES9318 / Savitech SA9023

- 证据等级为 `verified`。驱动表记录了 2026-11-22 在 ESP32-S3 v0.2、ESP-IDF 5.5.1、USB Host 1.4.1 上的公开 API 验证。[驱动表](../components/easy-esp-uacx/quirks/quirk_es9318.c) · [真实 FS 描述符](../components/easy-esp-uacx/test/fixtures/es9318_fs.h)
- UAC1 Full Speed 立体声 PCM。16 位使用 2 字节 subslot（MPS 400），24 位使用 3 字节 subslot（MPS 600）。两种位深均登记 32、44.1、48、88.2、96 kHz。
- 开流时先切到目标 alt，再向播放端点写入 3 字节采样率。Feature Unit 10 提供主通道静音和左右通道音量，专用驱动请求的初始音量为 0 dB。
- 当前登记的硬件验证覆盖 S3 FS。同一设备接到 HS 端口时会以 `262a:0001` 枚举，见下一节。播放中热插拔和 30 分钟播放尚无该设备的验收记录。

## 262a:0001 ES9318 / Savitech SA9023（HS）

- 同一块 ES9318 在 P4 HS Host 口上以不同 PID 枚举为 UAC2 设备。证据等级 `verified`：2026-11-22 P4 v1.3 / COM6 / HS，ESP-IDF 5.5.1、USB Host 1.4.1。[驱动表](../components/easy-esp-uacx/quirks/quirk_es9318.c) · [真实 HS 描述符](../components/easy-esp-uacx/test/fixtures/es9318_hs.h)
- AS 接口 2：alt 1/2/3 为 PCM16/24/32（subslot 2/3/4，MPS 200/300/400，bInterval 1），alt 4 为 RAW（不使用）。各 alt 共用异步反馈端点 0x84（MPS 4，bInterval 4 = 8 微帧）。登记 44.1、48、88.2、96、176.4、192、352.8、384 kHz。
- 反馈端点间隔为 8 微帧时，原先固定 8 包的反馈传输为 64 个调度槽，超过 IDF ISO 描述符表的 61 个可用槽，触发 `hcd_dwc.c` 断言并重启。现按 `包数 × 间隔 ≤ 61` 计算数据和反馈传输的包数；无法调度的端点在选择 alt 时被拒绝。
- 静音 SET 被接受，但 GET 始终返回 0。通用读回检测因此关闭静音，开流不做静音预热；音量读回正常，范围 −63 dB..0 dB / 1 dB。
- 验收：板上 43 项测试、跨任务检查、PCM16/24/32 各八档推送/拉取共 48 次通过；ISO 错误为 0、拉取欠载为 0，三次生命周期堆差为 0，`check_validation.py` 通过。听感、开流爆音、热插拔和 30 分钟播放尚未确认。

## 2fc6:f0fc CS43198（YUNJI43198PRO）

- 产品字符串为 `YUNJI43198PRO` 的 CS43198 小尾巴，在 P4 HS Host 口枚举为 UAC2 设备。证据等级 `verified`：2026-11-22 P4 v1.3 / HS，ESP-IDF 5.5.1、USB Host 1.4.1。[驱动表](../components/easy-esp-uacx/quirks/quirk_cs43198.c) · [真实 HS 描述符](../components/easy-esp-uacx/test/fixtures/cs43198_hs.h)
- AS 接口 1：alt 1/2/3 为 PCM16/24/32（subslot 2/3/4），alt 4 为 RAW（不使用）。数据端点 0x01 为异步 ISO OUT，bInterval 1，**每个 alt 的 MPS 都声明为 776 字节**；各 alt 共用异步反馈端点 0x81（MPS 4，bInterval 4 = 8 微帧）。登记 44.1、48、88.2、96、176.4、192、352.8、384 kHz。
- 776 字节超过 IDF 默认 BALANCED 分配下 512 字节的周期 TX FIFO，P4 需开启 `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`（已写入 `examples/p4/sdkconfig.defaults`）；否则该设备的所有 PCM alt 都无法调度。
- Feature Unit 3 提供主通道静音和左右声道音量，静音及音量读回均正常；驱动请求初始音量 −10.5 dB。仅精确匹配 `2fc6:f0fc`，不对该厂商做通配。
- 验收：公开 API PCM16/24/32 各八档推送/拉取共 48 次通过，音量和静音读回检测通过。2026-11-22 用户试听确认声音正常。开流爆音、热插拔和 30 分钟播放尚未确认。

### S3 FS 适配：失败

CS43198 **不支持 S3 FS**；条目不登记 FS 能力表，因此在 S3 上该设备能枚举，但不公布任何 PCM 能力。2026-11-22 在 S3 v0.2 / COM3 上测试：

- FS 下以相同的 `2fc6:f0fc` 枚举，描述符逐字节相同（仅一个配置，bcdDevice a0a0）。各 PCM alt 仍声明 MPS 776，超过 S3 600 字节周期 OUT FIFO，无法 claim 任何 alt。
- 实验一，把主机管道 MPS 限制为 582 字节（够 96 kHz PCM24）：PCM16/24 ≤96 kHz、PCM32 ≤48 kHz 推送/拉取 20/20 通过，ISO 错误为 0。FS 反馈为 10.14 格式且与采样率一致（44.1 kHz 时 0x0b0690 ≈ 44.04 kHz），播放中静音读回 0、音量 −10.5 dB。**但用户在所有采样率下都听不到声音。**
- 实验二，采用 USBPcap 抓到的 Windows usbaudio2 开流顺序（alt N → alt 0 → 设采样率 → alt N；电脑上该设备以 HS 运行，使用系统自带 `usbaudio2`，无厂商请求）：仍然无声。
- 两项实验均未对该设备启用。MPS 限制现以 `EUACX_DRV_FS_OUT_MPS` 的形式供下文 CS43131 使用。主机侧的传输、时钟和控制都正常，原因更可能在设备的 FS 模式（固件或供电），而非本驱动。未验证：该设备在 Windows FS 下（经仅 FS 的集线器）是否同样无声，以及 S3 VBUS 是否足以驱动其耳放。
- 早期两次采集中 S3 完全没有检测到连接事件；用户重新连接后枚举正常，原因未确定。

## 3302:98c2 CS43131（“CS43198 HiFi DSP Audio”）

- 产品字符串为 `CS43198 HiFi DSP Audio` 的 CS43131 小尾巴，在 S3 FS 和 P4 HS Host 口均以相同 ID、逐字节相同的描述符枚举为 UAC2 设备。证据等级 `verified`：2026-11-22 S3 v0.2 / COM3 / FS 及 P4 v1.3 / COM6 / HS，ESP-IDF 5.5.1、USB Host 1.4.1。[驱动表](../components/easy-esp-uacx/quirks/quirk_cs43131.c) · [真实 FS 描述符](../components/easy-esp-uacx/test/fixtures/cs43131_fs.h)
- 结构与上文 CS43198 相同：配置描述符 343 字节，AS 接口 1 的 alt 1/2/3 为 PCM16/24/32（subslot 2/3/4），alt 4 为 RAW；数据端点 0x07 为异步 ISO OUT，**每个 alt 都声明 MPS 776**；反馈端点 0x83（MPS 4，bInterval 4），Feature Unit 10，另有一个 HID 接口。
- 776 超过 S3 600 字节周期 OUT FIFO，条目设置 `EUACX_DRV_FS_OUT_MPS`，`fs_out_mps = 582`：FS 下在 claim 前把缓存的描述符和能力中的 MPS 降为 582。设备看不到这一限制；44.1 kHz PCM32 每帧最多 360 字节。
- `SET_INTERFACE alt 0` 之后设备对 `GET_INTERFACE` 返回 35，导致每次开流都以 `ESP_ERR_INVALID_RESPONSE` 失败。`EUACX_DRV_ALT_NO_READBACK` 改为信任成功的 SET_INTERFACE。
- **FS 下只有 44.1 kHz 能正确播放。** 时钟会列出并读回 48/88.2/96 kHz，矩阵在这些采样率下也 20/20 通过且 ISO 错误为 0，但用麦克风（UR22C Input 1/2）录到的输出：PCM16 48/96 kHz 静音，PCM16 88.2 kHz 为微弱噪声，PCM24/32 ≥ 48 kHz 为响亮的宽带噪声且音高不属于旋律。原因不在包大小（PCM32 44.1 kHz 与 PCM16 88.2 kHz 每帧都是 352.8 字节），也不在开流顺序（倒序采样率、先切 alt 后设采样率结果完全相同）；UAC1 端点采样率请求除 44.1 kHz 外全部 STALL。反馈端点返回零长度包，主机按标称速率发送。因此 FS 能力表对 PCM16/24/32 只登记 44.1 kHz。
- 静音读回失败，因此不提供静音；音量读回正常（−60 dB..0 dB / 0.5 dB）。驱动请求初始音量 −10.5 dB。
- **HS（P4）下八档采样率均可正常播放。** 776 字节需开启 `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`（与 CS43198 相同）。HS 能力表对 PCM16/24/32 登记 44.1、48、88.2、96、176.4、192、352.8、384 kHz。`EUACX_DRV_ALT_NO_READBACK` 在两种速度下都生效；MPS 限制仅用于 FS。
- FS 验收：板上 45 项测试；公开 API PCM16/24/32 在 44.1 kHz 推送/拉取共 6 次通过，ISO 错误为 0；录音显示六项旋律音高均正确。
- HS 验收：板上 48 项测试；公开 API PCM16/24/32 各八档推送/拉取共 48 次通过，ISO 错误为 0、拉取欠载为 0、堆差为 0，`check_validation.py` 通过。录音中 48 项的旋律与噪声电平一致，音高（C/G/A/F）正确；高频残留在 352.8/384 kHz 时由约 −31 dB 升至 −25 dB，仍比旋律低 50 dB 以上。
- 尚未测试：开流爆音、热插拔和 30 分钟播放。

## 2fc6:f800 ES9039Q2M（SXW-MDL7601-INTCLK_A2）

- 产品字符串为 `SXW-MDL7601-INTCLK_A2` 的 ES9039Q2M 解码板（厂商 ID 与上文 CS43198 相同），在 P4 HS Host 口枚举为 UAC2 设备。证据等级 `verified`：2026-11-22 P4 v1.3 / COM6 / HS，ESP-IDF 5.5.1、USB Host 1.4.1。[驱动表](../components/easy-esp-uacx/quirks/quirk_es9039.c) · [真实 HS 描述符](../components/easy-esp-uacx/test/fixtures/es9039_hs.h)
- 配置描述符 311 字节。AS 接口 1：alt 1/2/3 为 PCM16/24/32（subslot 2/3/4），alt 4 为 RAW；数据端点 0x01 为异步 ISO OUT，bInterval 1，**每个 alt 的 MPS 都是 776**，P4 需开启 `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`；反馈端点 0x81（MPS 4，bInterval 4 = 8 微帧）。登记 44.1、48、88.2、96、176.4、192、352.8、384 kHz。
- Feature Unit 3 在主通道上有静音和音量，左右通道上有音量（范围 −63.5 dB..0 dB / 0.5 dB）；静音和音量读回均正常，flags 为 0。按 UAC2，主通道与左右通道音量级联：三者都写 −10.5 dB 时录音实测衰减 −21 dB。现在库把设定音量写入主通道、左右通道保持单位增益（0 dB）；修改后录音电平比设备默认值恰好低 10.5 dB。
- 验收：板上 50 项测试；公开 API PCM16/24/32 各八档推送/拉取共 48 次通过，ISO 错误为 0、拉取欠载为 0、堆差为 0，`check_validation.py` 通过。用麦克风（UR22C Input 1/2）录音，48 项的旋律音高均正确且电平一致。录音中偶发的单帧（46 ms）高频突发按录音时间约每 7.7 s 重复一次，开播前也出现，且在各流中的位置随机，因此来自环境或录音通路，而非 DAC。
- 尚未测试：开流爆音、热插拔、30 分钟播放和 S3 FS。

## 2d99:a024 Edifier MF200（待确认 UAC2 的候选）
- 现象：公开报告称音量写入有效，但读当前音量返回固定值。
- 来源：[报告及设备 USB 日志](https://lkml.rescloud.iu.edu/hypermail/linux/kernel/2605.3/12239.html)、[补丁系列问题说明](https://patchew.org/linux/20260531-uac-quirk-get-cur-vol-v4-0-ede643dca151@rong.moe/)。只使用问题说明中的事实。
- 证据等级：reported（仅现象）；本项目未实测，也未取得可证明 UAC2 的描述符。
- 本项目条目：暂未加入。仅在确认 UAC2 播放接口后考虑 VOL_NO_READBACK；若确认仅 UAC1 则移除此候选记录。

## 152a:85dd SMSL USB DAC（候选）
- 现象：公开报告称额外的探测期接口设置以及从自动挂起恢复时有爆音。
- 来源：[2026-08-22 的问题报告](https://lkml.indiana.edu/hypermail/linux/kernel/2608.2/11659.html)。只记录 ID、现象和发生条件。
- 证据等级：reported；本项目未实测，具体型号及固件待采集。
- 本项目条目：暂未加入。当前枚举 claim 仅建立主机 pipe、不发送 SET_INTERFACE；本项目也没有运行时自动挂起。这一现象不足以启用本计划的延时/顺序标志，需要设备抓包和复现后再决定。

## 20b1:3008 iFi nano / micro iDSD（2.0 候选）
- 现象：设备作者的原生 DSD 支持项目记录这个 USB ID 可提供原生 DSD；同一 ID 可对应两种型号，不据此推断固件或字节/位序。
- 来源：[原生 DSD 支持项目的设备说明](https://github.com/lintweaker/xmos-native-dsd)；仅使用作者设备 ID 和能力描述，未导入实现或设备表。
- 证据等级：reported；本项目未实测。
- 本项目条目：暂未加入。首个原生 DSD 条目必须 verified，随 2.0 接入实际设备。

## 验收与录入

CX31993、ES9318、CS43198、CS43131 和 ES9039Q2M 已登记为 `verified`。CS43198 是 L3 首个新增的 UAC2 DAC 条目，仅在 P4 HS 上验证（听音已确认，热插拔和 30 分钟播放待确认）；S3 FS 适配失败，见上文。CS43131 在 S3 FS 上仅验证 44.1 kHz，在 P4 HS 上验证八档采样率（两者录音均已确认）。ES9039Q2M 在 P4 HS 上验证八档采样率（录音已确认）。新增候选时应先采集实际 VID:PID、描述符和问题复现。

对一个候选依次记录板型、完整描述符、bcdDevice、采样率/alt 请求顺序、控制写入和读回及听音/录音结果；完成矩阵及音量读回后更新等级。公开报告条目不带已实测能力表，猜测只保留本文件。
