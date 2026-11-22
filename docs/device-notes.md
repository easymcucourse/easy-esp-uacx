<!-- SPDX-FileCopyrightText: 2026 easymcucourse -->
<!-- SPDX-License-Identifier: MIT -->
# USB DAC 设备行为记录

本文件仅记录 USB ID、现象及条件、出处和本项目证据，不记录外部驱动的实现。协议依据 USB-IF Audio 2.0 第 4.9.2、5.2.6.1.2、A.17.11 节；同格式最小 MPS 行为也见 [微软 USB Audio 2.0 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)。UAC1 实机记录也列于此处。驱动条目按实际 USB ID 登记。

## 06cb:1594 CX31993
- 现象：FS/HS 的 alt 与 Host FIFO 限制不同；音量使用左右可写通道，静音使用 master。
- 来源：[本项目验收记录](../plan.zh.md)、[真实 FS 描述符](../components/easy-esp-uacx/test/fixtures/cx31993_fs.h)。
- 证据等级：verified（现有条目）。
- 本项目条目：CX31993，flags = 0，保持既有 −10.5 dB 初始音量及 FS/HS 能力表。
- 本次回归：2026-10-06 P4 v1.3 / COM10 / HS，39 项板上测试和 PCM16/24/32 各八档推送/拉取共 48 次通过；全部 ISO 错误为 0、拉取欠载为 0，音量和静音读回检测通过，用户确认声音正常。S3 新控制逻辑回归仍待完成。

## 262a:0002 ES9318 / Savitech SA9023

- 证据等级为 `verified`。驱动表记录了 2026-11-22 在 ESP32-S3 v0.2、ESP-IDF 5.5.1、USB Host 1.4.1 上的公开 API 验证。[驱动表](../components/easy-esp-uacx/drivers/drv_es9318.c) · [真实 FS 描述符](../components/easy-esp-uacx/test/fixtures/es9318_fs.h)
- UAC1 Full Speed 立体声 PCM。16 位使用 2 字节 subslot（MPS 400），24 位使用 3 字节 subslot（MPS 600）。两种位深均登记 32、44.1、48、88.2、96 kHz。
- 开流时先切到目标 alt，再向播放端点写入 3 字节采样率。Feature Unit 10 提供主通道静音和左右通道音量，专用驱动请求的初始音量为 0 dB。
- 当前登记的硬件验证覆盖 S3 FS。P4、播放中热插拔和 30 分钟播放尚无该设备的验收记录。

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

CX31993 和 ES9318 已登记为 `verified`。L3 仍需验证至少一台新的 UAC2 DAC。新增候选时应先采集实际 VID:PID、描述符和问题复现。

对一个候选依次记录板型、完整描述符、bcdDevice、采样率/alt 请求顺序、控制写入和读回及听音/录音结果；完成矩阵及音量读回后更新等级。公开报告条目不带已实测能力表，猜测只保留本文件。
