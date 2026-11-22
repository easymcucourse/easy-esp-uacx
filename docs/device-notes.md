<!-- SPDX-FileCopyrightText: 2026 easymcucourse -->
<!-- SPDX-License-Identifier: MIT -->
# USB DAC Device Behavior Notes

English | [中文](device-notes.zh.md)

This file records only USB IDs, symptoms and conditions, sources, and this project's evidence; it does not record how external drivers implement anything. The protocol basis is USB-IF Audio 2.0 sections 4.9.2, 5.2.6.1.2, and A.17.11; the smallest-MPS behavior for the same format is also described in [Microsoft's USB Audio 2.0 documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers). UAC1 hardware records are listed here too. Driver entries are registered by actual USB ID.

## 06cb:1594 CX31993
- Symptom: FS and HS differ in alts and host FIFO limits; volume uses the writable left/right channels, mute uses master.
- Sources: [this project's acceptance record](../plan.zh.md) (Chinese), [real FS descriptor](../components/easy-esp-uacx/test/fixtures/cx31993_fs.h).
- Evidence level: verified (existing entry).
- Project entry: CX31993, flags = 0, keeping the existing −10.5 dB initial volume and FS/HS capability tables.
- Latest regression: 2026-10-06 P4 v1.3 / COM10 / HS — 39 on-board tests and push/pull at eight rates each for PCM16/24/32, 48 runs in total, passed; all ISO errors 0, pull underruns 0, volume and mute readback probes passed, and the user confirmed normal sound. S3 regression of the new control logic is still pending.

## 262a:0002 ES9318 / Savitech SA9023

- Evidence level `verified`. The driver table records public-API verification on 2026-11-22 with ESP32-S3 v0.2, ESP-IDF 5.5.1, USB Host 1.4.1. [Driver table](../components/easy-esp-uacx/quirks/quirk_es9318.c) · [Real FS descriptor](../components/easy-esp-uacx/test/fixtures/es9318_fs.h)
- UAC1 Full Speed stereo PCM. 16-bit uses a 2-byte subslot (MPS 400), 24-bit a 3-byte subslot (MPS 600). Both bit depths register 32, 44.1, 48, 88.2, and 96 kHz.
- Stream open first switches to the target alt, then writes the 3-byte sample rate to the playback endpoint. Feature Unit 10 provides master mute and left/right volume; the driver requests an initial volume of 0 dB.
- Registered hardware verification currently covers S3 FS. The same device on an HS port enumerates as `262a:0001`; see the next section. There is no acceptance record yet for hot-plug during playback or 30-minute playback with this device.

## 262a:0001 ES9318 / Savitech SA9023 (HS)

- The same ES9318 board enumerates on the P4 HS host port with a different PID as a UAC2 device. Evidence level `verified`: 2026-11-22 P4 v1.3 / COM6 / HS, ESP-IDF 5.5.1, USB Host 1.4.1. [Driver table](../components/easy-esp-uacx/quirks/quirk_es9318.c) · [Real HS descriptor](../components/easy-esp-uacx/test/fixtures/es9318_hs.h)
- AS interface 2: alt 1/2/3 are PCM16/24/32 (subslot 2/3/4, MPS 200/300/400, bInterval 1); alt 4 is RAW (unused). All alts share asynchronous feedback endpoint 0x84 (MPS 4, bInterval 4 = 8 microframes). Registered rates: 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz.
- With an 8-microframe feedback interval, the previous fixed 8-packet feedback transfer needed 64 schedule slots, exceeding the 61 usable slots in IDF's ISO descriptor list, which hit an assertion in `hcd_dwc.c` and rebooted. Packet counts for data and feedback transfers are now computed so that `packets × interval ≤ 61`; endpoints that cannot be scheduled are rejected during alt selection.
- Mute SET is accepted but GET always returns 0. The generic readback probe therefore disables mute, and stream open skips the mute warm-up; volume readback works, range −63 dB..0 dB / 1 dB.
- Acceptance: 43 on-board tests, cross-task checks, and push/pull at eight rates each for PCM16/24/32, 48 runs in total, passed; ISO errors 0, pull underruns 0, heap delta 0 over three lifecycles, and `check_validation.py` passed. Listening quality, pops at stream open, hot-plug, and 30-minute playback are not yet confirmed.

## 2fc6:f0fc CS43198 (YUNJI43198PRO)

- A CS43198 dongle with product string `YUNJI43198PRO`, enumerating as a UAC2 device on the P4 HS host port. Evidence level `verified`: 2026-11-22 P4 v1.3 / HS, ESP-IDF 5.5.1, USB Host 1.4.1. [Driver table](../components/easy-esp-uacx/quirks/quirk_cs43198.c) · [Real HS descriptor](../components/easy-esp-uacx/test/fixtures/cs43198_hs.h)
- AS interface 1: alt 1/2/3 are PCM16/24/32 (subslot 2/3/4); alt 4 is RAW (unused). Data endpoint 0x01 is asynchronous ISO OUT, bInterval 1, and **every alt declares a 776-byte MPS**; all alts share asynchronous feedback endpoint 0x81 (MPS 4, bInterval 4 = 8 microframes). Registered rates: 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz.
- 776 bytes exceeds the 512-byte periodic TX FIFO of IDF's default BALANCED split, so P4 needs `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` (set in `examples/p4/sdkconfig.defaults`); without it none of the device's PCM alts can be scheduled.
- Feature Unit 3 provides master mute and left/right volume; mute and volume readback both work. The driver requests an initial volume of −10.5 dB. Only `2fc6:f0fc` is matched exactly; there is no vendor wildcard.
- Acceptance: public-API push/pull at eight rates each for PCM16/24/32, 48 runs in total, passed; volume and mute readback probes passed; the user confirmed by listening on 2026-11-22 that playback sounds normal. Pops at stream open, hot-plug, and 30-minute playback are not yet confirmed.

### S3 FS adaptation: failed

The CS43198 is **not supported on S3 FS**; the entry registers no FS table, so on S3 the dongle enumerates but publishes no PCM capability. Tested 2026-11-22 on S3 v0.2 / COM3:

- At FS it enumerates with the same `2fc6:f0fc` and a byte-identical descriptor (one configuration, bcdDevice a0a0). Every PCM alt still declares MPS 776, which S3's 600-byte periodic OUT FIFO rejects, so no alt can be claimed.
- Experiment 1, capping the host pipe MPS at 582 bytes (enough for 96 kHz PCM24): PCM16/24 ≤96 kHz and PCM32 ≤48 kHz streamed 20/20 with zero ISO errors. The FS feedback was 10.14 and matched the rate (0x0b0690 ≈ 44.04 kHz at 44.1 kHz), and during playback mute read 0 and volume −10.5 dB. **The user heard silence at every rate.**
- Experiment 2, Windows usbaudio2's open order (alt N → alt 0 → rate → alt N, captured with USBPcap; on the PC the dongle runs at HS with the inbox `usbaudio2` and no vendor requests): still silent.
- Neither experiment is enabled for this device. The MPS cap now exists as `EUACX_DRV_FS_OUT_MPS`, used by the CS43131 below. The host-side transfer, clock and controls looked correct, so the cause is most likely in the dongle at FS (firmware or power), not in this driver. Untested: whether the dongle is also silent at FS under Windows (via an FS-only hub), and whether S3 VBUS is sufficient for its headphone amp.
- In two early captures the S3 saw no connect event at all; after the user reconnected the dongle, enumeration was normal. Cause not determined.

## 3302:98c2 CS43131 ("CS43198 HiFi DSP Audio")

- A CS43131 dongle whose product string reads `CS43198 HiFi DSP Audio`, enumerating as a UAC2 device on the S3 FS and P4 HS host ports with the same ID and a byte-identical descriptor. Evidence level `verified`: 2026-11-22 S3 v0.2 / COM3 / FS and P4 v1.3 / COM6 / HS, ESP-IDF 5.5.1, USB Host 1.4.1. [Driver table](../components/easy-esp-uacx/quirks/quirk_cs43131.c) · [Real FS descriptor](../components/easy-esp-uacx/test/fixtures/cs43131_fs.h)
- Same layout as the CS43198 above: 343-byte configuration, AS interface 1 alt 1/2/3 PCM16/24/32 (subslot 2/3/4), alt 4 RAW, data endpoint 0x07 asynchronous ISO OUT declaring **MPS 776 on every alt**, feedback endpoint 0x83 (MPS 4, bInterval 4), Feature Unit 10, plus a HID interface.
- 776 exceeds S3's 600-byte periodic OUT FIFO, so the entry sets `EUACX_DRV_FS_OUT_MPS` with `fs_out_mps = 582`: at FS the cached descriptor and caps are lowered to 582 before claim. The device never sees the cap; 44.1 kHz PCM32 needs at most 360 bytes per frame.
- After `SET_INTERFACE alt 0` the device answers `GET_INTERFACE` with 35, so every stream open failed with `ESP_ERR_INVALID_RESPONSE`. `EUACX_DRV_ALT_NO_READBACK` trusts a successful SET_INTERFACE instead.
- **Only 44.1 kHz plays at FS.** The clock lists and reads back 48/88.2/96 kHz, and the matrix passed 20/20 at all of them with zero ISO errors, but the output recorded with a microphone (UR22C Input 1/2) was muted at PCM16 48/96 kHz, faint noise at PCM16 88.2 kHz, and loud broadband noise with non-melody pitches at PCM24/32 ≥ 48 kHz. It is not packet size (PCM32 44.1 kHz and PCM16 88.2 kHz both send 352.8 bytes per frame) and not open order (reversed rate order and alt-before-rate gave identical results); the UAC1 endpoint rate request STALLs for every rate except 44.1 kHz. The feedback endpoint returns zero-length packets, so the host sends at the nominal rate. The FS table therefore registers 44.1 kHz only for PCM16/24/32.
- Mute readback fails, so mute is not offered; volume readback works (−60 dB..0 dB / 0.5 dB). The driver requests an initial volume of −10.5 dB.
- **HS (P4) plays all eight rates.** 776 bytes needs `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` (as for CS43198). The HS table registers 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz for PCM16/24/32. `EUACX_DRV_ALT_NO_READBACK` applies at both speeds; the MPS cap is FS-only.
- FS acceptance: 45 on-board tests; public-API push/pull at 44.1 kHz for PCM16/24/32, 6 runs, passed with ISO errors 0; the recording shows the correct melody pitches in all six.
- HS acceptance: 48 on-board tests; public-API push/pull for PCM16/24/32 at eight rates, 48 runs, passed with ISO errors 0, pull underruns 0, heap delta 0, and `check_validation.py` passed. In the recording all 48 cases have the same tone and noise levels and the melody pitches (C/G/A/F); high-frequency residue rises from about −31 dB to −25 dB at 352.8/384 kHz, still more than 50 dB below the tone.
- Not yet tested: pops at stream open, hot-plug, and 30-minute playback.

## 2fc6:f800 ES9039Q2M (SXW-MDL7601-INTCLK_A2)

- An ES9039Q2M DAC board with product string `SXW-MDL7601-INTCLK_A2` (same vendor ID as the CS43198 above), enumerating as a UAC2 device on the P4 HS host port. Evidence level `verified`: 2026-11-22 P4 v1.3 / COM6 / HS, ESP-IDF 5.5.1, USB Host 1.4.1. [Driver table](../components/easy-esp-uacx/quirks/quirk_es9039.c) · [Real HS descriptor](../components/easy-esp-uacx/test/fixtures/es9039_hs.h)
- 311-byte configuration. AS interface 1: alt 1/2/3 PCM16/24/32 (subslot 2/3/4), alt 4 RAW; data endpoint 0x01 asynchronous ISO OUT, bInterval 1, **MPS 776 on every alt**, so P4 needs `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`; feedback endpoint 0x81 (MPS 4, bInterval 4 = 8 microframes). Registered rates: 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz.
- Feature Unit 3 has mute and volume on master plus volume on left/right (range −63.5 dB..0 dB / 0.5 dB); mute and volume readback both pass, flags 0. Master and channel volume cascade per UAC2: writing −10.5 dB to all three measured −21 dB on the recording. The library now puts the requested volume on master and holds the channels at unity (0 dB); afterwards the recording dropped exactly 10.5 dB from the device default.
- Acceptance: 50 on-board tests; public-API push/pull for PCM16/24/32 at eight rates, 48 runs, passed with ISO errors 0, pull underruns 0, heap delta 0, and `check_validation.py` passed. The output recorded with a microphone (UR22C Input 1/2) shows the correct melody pitches at the same level in all 48 cases. Single 46 ms high-frequency bursts in the recording recur about every 7.7 s of recording time, also before playback starts and at random offsets within streams, so they come from the room or recording path, not the DAC.
- Not yet tested: pops at stream open, hot-plug, 30-minute playback, and S3 FS.

## 2d99:a024 Edifier MF200 (candidate, UAC2 unconfirmed)
- Symptom: a public report says volume writes take effect, but reading the current volume returns a fixed value.
- Sources: [report and device USB log](https://lkml.rescloud.iu.edu/hypermail/linux/kernel/2605.3/12239.html), [patch series problem statement](https://patchew.org/linux/20260531-uac-quirk-get-cur-vol-v4-0-ede643dca151@rong.moe/). Only facts from the problem statement are used.
- Evidence level: reported (symptom only); not tested by this project, and no descriptor proving UAC2 has been obtained.
- Project entry: not added. VOL_NO_READBACK will be considered only after a UAC2 playback interface is confirmed; if it turns out to be UAC1 only, this candidate record will be removed.

## 152a:85dd SMSL USB DAC (candidate)
- Symptom: a public report describes pops on extra probe-time interface settings and on resume from autosuspend.
- Source: [problem report of 2026-08-22](https://lkml.indiana.edu/hypermail/linux/kernel/2608.2/11659.html). Only the ID, symptom, and trigger conditions are recorded.
- Evidence level: reported; not tested by this project; exact model and firmware still to be collected.
- Project entry: not added. Enumeration claims currently only set up host pipes and send no SET_INTERFACE, and this project has no runtime autosuspend. The symptom is not enough to enable this plan's delay/ordering flags; decide after a device capture and reproduction.

## 20b1:3008 iFi nano / micro iDSD (2.0 candidate)
- Symptom: a device author's native-DSD support project records that this USB ID can provide native DSD; the same ID may map to two models, so no firmware or byte/bit order is inferred from it.
- Source: [device notes of the native DSD support project](https://github.com/lintweaker/xmos-native-dsd); only the author's device ID and capability description are used, with no implementation or device table imported.
- Evidence level: reported; not tested by this project.
- Project entry: not added. The first native DSD entry must be verified and will be added with real hardware in 2.0.

## Acceptance and admission

CX31993, ES9318, CS43198, CS43131, and ES9039Q2M are registered as `verified`. CS43198 is the first new UAC2 DAC entry for L3, verified on P4 HS only (listening confirmed; hot-plug and 30-minute playback pending); its S3 FS adaptation failed, see above. CS43131 is verified on S3 FS at 44.1 kHz only and on P4 HS at eight rates (recorded output confirmed for both). ES9039Q2M is verified on P4 HS at eight rates (recorded output confirmed). Before adding a new candidate, collect the actual VID:PID, descriptors, and a reproduction of the problem.

For each candidate, record in turn the board, full descriptors, bcdDevice, sample-rate/alt request order, control writes and readbacks, and listening/recording results; update the evidence level after the matrix and volume readback are complete. Public-report entries carry no hardware-tested capability table, and guesses stay in this file only.
