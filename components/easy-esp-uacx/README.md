# easy UACX Component API

English | [中文](README.zh.md)

Current version: **1.0.0-rc.1**. The PCM runtime is implemented; hardware acceptance progress for the official release is tracked in the [root README](../../README.md) and the [examples guide](../../examples/README.md). The only public header is `easy_uacx.h`; applications do not need to include USB, FreeRTOS, or driver-table headers.

## Initialization, ports, and capabilities

```c
euacx_config_t config = EUACX_CONFIG_DEFAULT();
config.cb.on_connected = connected;
config.cb.on_disconnected = disconnected;
config.cb.on_stream_stopped = stopped;
ESP_ERROR_CHECK(euacx_init(&config));
```

`euacx_init` creates the manager, the USB event pump, and the status-callback task, and by default installs the USB Host library and its library-event task. Calling it again returns `ESP_ERR_INVALID_STATE`. Task priorities must be lower than the pump's; core IDs must be `-1` or a valid CPU number. The feed task priority must also be lower than the pump's.

`euacx_port_count()` currently returns 1. `euacx_get_port(0)` returns the default root-port handle; out-of-range indices return NULL. Handles stay valid between init and deinit and do not change across unplug/replug; do not keep using an old handle after deinit. S3 uses the default FS host and P4 the default HS host; the second P4 FS root port is not implemented.

`euacx_get_info` copies a capability snapshot and succeeds only in the CONNECTED/STREAMING states. Each enumeration increments `conn_id`; applications handling delayed messages should check it. `pcm[0/1/2]` are ascending rate lists for **16/24/32-bit input** respectively; the converted device container may be wider. The default verified mode uses the intersection of the driver table and descriptor/bandwidth limits; devices without a dedicated driver use the generic descriptor capabilities. When UAC2 sample-rate GET RANGE is unavailable, standard candidate rates are published and confirmed by SET/GET at stream open. UAC1 reads discrete rates from the format descriptor; stream open first enables the alt, then sends a 3-byte `SET_CUR` to the playback endpoint, and judges success by the request result.

## PCM data and playback

| Input | Stereo frame size | Layout |
|---|---|---|
| 16-bit | 4 bytes | L16, R16, little-endian signed |
| 24-bit | 6 bytes | packed L24, R24, little-endian signed |
| 32-bit | 8 bytes | L32, R32, little-endian signed |

The driver picks the narrowest device format that can hold the input bit depth. Conversion to a wider container is MSB-aligned with zero-filled low bits; it never reduces bit depth or resamples. 24-bit input mapped into a 4-byte container has a zero lowest byte. In verified mode, stream open keeps the subslot registered in the table, so a capability check that passes cannot pick a different, unverified container.

During stream open, hardware mute protects the clock/alt switch: the silent ISO pipeline is submitted first, then the application's previous mute state is restored and PCM consumption starts. Mute also protects the switch back to alt0 on close. During enumeration each alt is temporarily claimed/released to detect actual controller/FIFO limits, instead of filtering by theoretical USB bus bandwidth alone.

Push mode:

```c
euacx_stream_config_t stream = {
    .format = EUACX_FORMAT_PCM, .sample_rate = 48000,
    .bits = 24, .channels = 2,
};
ESP_ERROR_CHECK(euacx_stream_open(port, &stream, NULL));
size_t offset = 0;
while (offset < size) {
    size_t written = 0;
    esp_err_t e = euacx_write(port, data + offset, size - offset, &written, 1000);
    offset += written;
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) break;
    // A timeout may accept a prefix; retry only the unwritten suffix.
}
ESP_ERROR_CHECK(euacx_stream_close(port));
```

Only the task that opened the stream may write/close a push stream. Writes are copied into an internal buffer, so the application can reuse its memory after return; `written` includes accepted bytes of incomplete frames. Timeouts are in ms: 0 means no wait, `UINT32_MAX` waits forever. Input may span calls to form frames. Close stops immediately and discards incomplete frames and unplayed buffers; push mode has no EOF-drain interface.

Pull mode sets `on_data` and `data_user` in the config. The driver calls `on_data(port, buf, len, user)` from the dedicated `euacx_feed` task: return 1..len input bytes, 0 for no data yet, or `EUACX_DATA_END` to finish. Feeding need not be frame-aligned; at the end a partial frame is zero-padded, and `STOP_EOF` is sent once buffers and in-flight transfers drain. Any other negative value, or a value larger than len, causes `STOP_ERROR`. The callback should return quickly; on 0 the driver retries, and a running buffer that runs short sends silence and counts an underrun.

| Calling context | Rules |
|---|---|
| Status-callback task `euacx_cb` | Runs connected/stopped/disconnected in order and may call public APIs; the info pointer is valid only inside that callback |
| Task owning a push stream | write, close; may supply any number of bytes per call |
| Other application tasks | May query, adjust volume, and abort; may not write/close another task's push stream |
| Pull feed task | May not write, close/open the same port, or deinit; may query and adjust volume |
| Other tasks of a pull stream | May close; completes after the feed callback exits and USB transfers are cancelled |

`euacx_stream_abort` does not wait. In push mode it wakes a blocked write, which returns `ESP_ERR_INVALID_STATE`; the stream stays STREAMING until the owning task closes it. In pull mode it requests a close. abort/close may be called repeatedly when no stream exists. If a status callback blocks for a long time, notifications are queued while the manager and USB pump keep running; terminal-notification nodes are pre-reserved on successful stream open/enumeration, and when the queue is full they overflow in FIFO order.

`euacx_stream_close` waits synchronously for the stream to be released. If the feed callback blocks, the close request completes late while the manager still handles other requests (e.g. volume); the application must let the callback eventually return. `euacx_deinit` stops streams, releases the device and tasks, and uninstalls only a host it installed itself; when called from a status callback, the final memory release is deferred until the callback exits. deinit never force-kills a feed callback the application is running.

## States and errors

State path: `DISCONNECTED → ENUMERATING → CONNECTED ⇄ STREAMING`, returning to DISCONNECTED on unplug. Unplug, reaching the consecutive-ISO-error threshold, close, or EOF sends stopped once for a successfully opened stream; a device that sent a connected notification then sends disconnected with the matching `conn_id`. Stop reasons are `EOF/CLOSED/UNPLUGGED/ERROR`; only ERROR carries a stream error.

| Error | Meaning |
|---|---|
| `ESP_ERR_INVALID_ARG` | Null argument, invalid init config, or invalid PCM format parameters |
| `ESP_ERR_INVALID_STATE` | Not initialized, not connected, stream already open, write after abort, wrong task, etc. |
| `ESP_ERR_NOT_SUPPORTED` | Unsupported bit depth/sample rate, no volume/mute control, DSD/DoP |
| `ESP_ERR_TIMEOUT` | Write waiting for space, or API request enqueue timeout; a control-transfer timeout also triggers device recovery |
| `ESP_ERR_NO_MEM` | Allocation failure for stream buffers, transfers, notifications, or tasks |

Once an API request is enqueued, the caller waits for the manager's reply, so a request referencing caller stack memory never times out early. Internal USB events use address/unplug flags as a fallback when the queue is full and take priority over application requests. Control transfers have their own heap memory and reference count and are freed after the USB completion callback even if they timed out.

## Hardware volume and mute

`has_volume/has_mute` indicate the control is readable and writable on the playback path. Volume is signed **1/256 dB**, e.g. −10.5 dB = −2688. set clamps to min/max and aligns down to res, sends SET to the supported master/left/right channels, then reads back with GET to confirm. When master and channel volume are both writable they cascade (UAC2), so the value goes on master and the channels are set to unity (0 dB, clamped to the range); get returns the first supported channel. With multiple RANGE subranges, the first is used with a warning; res 0 is treated as 1/256 dB, and a fixed or invalid range disables hardware volume. During enumeration an adjacent-step probe runs on the first read/write channel and the original value is restored; untrustworthy readback disables the corresponding control. Volume/mute failures neither block enumeration nor stop playback (a device drop is still handled as a normal disconnect).

UAC1 volume range uses Feature Unit `GET_MIN`, `GET_MAX`, `GET_RES`; the current value uses `GET_CUR`. The ES9318 driver matches FS `262a:0002` and HS `262a:0001` (UAC2), with an initial volume of 0 dB.

The CX31993 driver matches only `06cb:1594`, with an initial volume of −10.5 dB; the generic probe restores the device's original volume. Devices declared to have untrustworthy readback get the lowest usable volume written at enumeration to seed the cache, without reading CUR. Supported mute controls are unmuted at enumeration. Non-ASCII product strings are currently replaced with `?`.

## Configuration

| Option | Default | Description |
|---|---|---|
| `EUACX_DRV_CX31993` | y | Dedicated driver |
| `EUACX_DRV_ES9318` | y | ES9318 / SA9023 driver with S3 FS and P4 HS verified capability tables |
| `EUACX_DRV_CS43198` | y | CS43198 (`2fc6:f0fc`) driver with P4 HS verified capability table (silent on S3 FS, see device notes); P4 needs `USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` |
| `EUACX_DRV_CS43131` | y | CS43131 (`3302:98c2`) driver with S3 FS (44.1 kHz only) and P4 HS (8 rates) verified tables; caps the FS ISO OUT MPS at 582 and skips GET_INTERFACE readback; P4 needs `USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` |
| `EUACX_DRV_ES9039` | y | ES9039Q2M (`2fc6:f800`) driver with P4 HS verified capability table (silent on S3 FS, see device notes); P4 needs `USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` |
| `EUACX_DRV_REPORTED` | y | Allow entries backed by public reports; no new entry currently meets all admission criteria |
| `EUACX_USE_VERIFIED_CAPS` | y | Use verified capability tables (when present) |
| `EUACX_BUFFER_MS` | 40 | Minimum buffer duration; grown to pipeline capacity and a power of two |
| `EUACX_NUM_TRANSFERS` | 4 | In-flight ISO transfers, 2–8 |
| `EUACX_MAX_ERRORS` | 8 | Consecutive ISO error threshold for stopping |
| `EUACX_TASK_STACK` | 8192 | Manager stack in bytes |
| `EUACX_LIB_PRIORITY` | 20 | Priority of the self-installed host library task |
| `EUACX_PUMP_PRIORITY/STACK` | 19 / 8192 | USB completion event and initial submission task |
| `EUACX_INT_QUEUE_LEN` | 8 | Internal USB event queue length |
| `EUACX_REQ_QUEUE_LEN/TIMEOUT_MS` | 8 / 2000 | Application request queue and enqueue wait |
| `EUACX_CB_STACK/QUEUE_LEN` | 4096 / 16 | Status-callback task stack and notification queue |
| `EUACX_FEED_PRIORITY/STACK` | 10 / 8192 | Pull feed task |
| `EUACX_FEED_RETRY_MS` | 2 | Retry wait after returning 0 (plus one scheduler tick) |
| `EUACX_RECOVERY_MS` | 5000 | Root-port recovery period with no DAC; 0 disables |
| `EUACX_DUMP_DESCRIPTORS` | n | Print accepted configuration descriptors to build real test fixtures |

The only public dependency is `esp_common`; USB/FreeRTOS/heap/esp_rom are private dependencies. USB Host is pinned to 1.4.1, IDF ≥ 5.4, currently verified with 5.5.1. In external-host mode the application must install USB Host first and keep pumping library events.

## Adding drivers and tests

Generic UAC drivers and matching logic live in `drivers/`. A device adaptation defines its capability table and behavior parameters in `quirks/quirk_<name>.c`, declares its entry in `quirk_entries.h` in the same directory, registers it in `quirk_table.c`, and adds Kconfig/CMake conditions as needed. Exact VID:PID beats vendor wildcards, with generic last; within one product, an entry with a matching bcdDevice range beats one without. Control and stream quirks must be product-exact; vendor wildcards are not allowed. Drivers may use attach/detach, fixup_caps, and stream_start/stop hooks; these run on the manager and must not call public APIs synchronously.

Verified tables are split by FS/HS and record input bit depth, actual subslot, and strictly ascending sample rates; only combinations checked by actual listening or recording, with transfers passing, are entered. `euacx_driver_validate` checks entries. The CX31993 FS table registers this round's S3 PCM16 (subslot 2) and PCM24 (subslot 3) at 8/16/32/44.1/48/96 kHz; the HS table registers P4 PCM16/24/32 (subslot 2/3/4) at eight rates each, including 192/384 kHz, verified by 48 playback runs compared against UR22C input 1/2 recordings. The FS PCM32 endpoint's MPS 768 exceeds the S3's measured FIFO of 600, so the enumeration-time host claim probe excludes it.

`test_pcm_model.c` includes a real CX31993 FS descriptor fixture and tests the playback clock/FU, bandwidth, verified-table/stream-open consistency, PCM bytes, feedback, RANGE, state permissions, and counter wraparound. The host script also compiles the production control module, substituting transfers/delays/interface claims to test device behavior; on-board tests exercise repeated lifecycles of the real USB library and frame assembly/timeout/abort in the production write path. The examples use the public API to verify cross-task behavior, the full matrix, and long-duration playback.

Explicit feedback currently has algorithm tests but still needs hardware acceptance with an asynchronous DAC; implicit feedback, complex clock routing, and the optional P4 dual root port are not supported. The old DoP/native DSD algorithms and tests are kept as a private reference for 2.0 and are not wired into the 1.0 playback API.

## Device quirks and contribution guidelines

The L0–L2 software parts follow the [Linux reference plan](../../plan-linux-quirks.zh.md) (Chinese); L3 hardware testing of new devices and L4 native DSD are still pending. Device facts and sources are kept in [device-notes](../../docs/device-notes.md); code keeps only names and parameters. `info.driver_flags` shows the behavior of the matched entry; flag definitions and params live in the private `euacx_driver.h`.

| Flag (`EUACX_DRV_` prefix) | Behavior |
|---|---|
| `VOL_NO_READBACK` | Volume/mute skips CUR readback; get returns the cache of the last successful write. The cache is invalidated after a partial channel write failure |
| `VOL_MIN_IS_MUTE` | Raise the published minimum volume by one step; setting to the device's original minimum or lower mutes instead. Requires writable mute |
| `VOL_RANGE` | params supply min/max/res, skipping the device RANGE |
| `NO_HW_VOLUME` | Do not publish hardware volume |
| `CTL_DELAY` | Wait 1–20000 μs after each class request; millisecond delays round up to scheduler ticks |
| `RATE_NO_READBACK` | Skip the frequency CUR comparison after setting the rate, keeping the clock-validity check |
| `ALT_BEFORE_RATE` | alt 0 → alt N → set sample rate |
| `IFACE_DELAY` | Wait 1–200 ms after SET_INTERFACE to a non-zero alt |

The default UAC2 order is alt 0 → set and read back sample rate → alt N. Among PCM alts with the same bit depth/subslot, the one with the smallest usable MPS is chosen; if the device declares a valid-alt control, after setting the frequency the bitmap (with length byte) is read to exclude invalid candidates before trying the next one. On failure the driver returns to alt 0 and releases the interface. Buffers and ISO transfers are sized for the finally selected alt.

For a new entry, write the fact record first, then implement independently. `verified` requires this project's own S3/P4 testing, and capability tables additionally need listening or recording plus transfer verification. `reported` requires a public report containing the USB ID and symptom, sets `.reported = true`, is gated by `EUACX_DRV_REPORTED`, and the connection log states the evidence level. `guess` goes into documentation only. Do not add an entry when the protocol version is undetermined or the symptom does not map to one of this project's flags. Each new device entry should be committed on its own, with the symptom and evidence in the commit message.

This project is MIT-licensed and does not accept Linux GPL code, headers, macros, device tables, or translations/paragraph-by-paragraph rewrites of them; do not build code correspondences on Linux internal names, and do not open its source for comparison while writing an implementation. Linux community reports are used only to extract device facts; the protocol implementation follows the USB-IF specifications; no source from other projects is imported. On 2026-10-06, 55 host tests passed in both the default configuration and the configuration with CX31993/reported disabled, and the S3/P4 examples compiled; afterwards 39 on-board tests on P4 v1.3 and 48 CX31993 matrix playbacks passed, with zero errors/zero underruns on pull and the user confirming normal sound. S3 and the new quirk devices are still pending acceptance. Host tests first run `scripts/check_license.ps1` and eight violation samples, scanning source files in the project's own components/examples/scripts and excluding third-party code, docs, and build artifacts.
