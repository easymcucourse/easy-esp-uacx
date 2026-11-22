# easy-esp-uacx

English | [中文](README.zh.md)

USB stereo PCM playback driver for ESP32-S3 (FS) and ESP32-P4 (HS), supporting UAC2 and the ES9318 (UAC1 on S3 FS, UAC2 on P4 HS). Applications include only `easy_uacx.h` to query DAC capabilities, push or pull PCM, adjust hardware volume, and receive status notifications.

## Status

**1.0.0-rc.1, hardware acceptance in progress.** The PCM runtime is implemented and the examples have moved to the public API. The official `v1.0.0` release still requires the new driver's playback matrix, listening tests, unplug/replug recovery, and a 30-minute playback run; these results will not be substituted with records from the historical reference program.

| Feature | Implementation |
|---|---|
| Public API | Init/deinit, persistent port, state/capability snapshots, connect/disconnect/stop callbacks |
| PCM | 16/24/32-bit interleaved little-endian stereo, push/pull, frame assembly across calls, conversion to higher bit depths |
| USB | UAC2 playback clock and RANGE, UAC1 discrete sample rates and endpoint frequency setting, SET/GET INTERFACE, ISO scheduling, explicit feedback endpoint |
| Drivers | Exact VID:PID → vendor wildcard → generic; a separate device instance per enumeration |
| Volume | Volume/mute control on the playback-path Feature Unit, range clamping, step alignment and readback |
| Lifecycle | Serialized state changes in the manager, separate USB/callback/data tasks, abort wakeup, deferred close, enumeration retry |
| Reserved for 2.0 | DSD/DoP types are defined; 1.0 reports no such capability and opening a stream returns `ESP_ERR_NOT_SUPPORTED` |

Verified 2026-10-05: host-only algorithm tests pass **33 cases** each with CX31993 enabled and disabled; **37 tests pass** on S3 hardware and **39 tests pass** on P4 hardware, including repeated init/deinit, heap recovery, real bulk writes, and request congestion. Cross-task permission/abort checks, and volume and close checks while the pull callback is blocked, pass on both boards. S3 PCM16/24 across six configurations (24 runs) and P4 PCM16/24/32 across eight configurations (48 runs) all play successfully, with zero errors/zero underruns in pull mode. The user confirmed the S3 startup pop is gone; the P4 384 kHz noise fix was checked against input 1/2 recordings. The full 30-minute run is still pending.

Host regression for the current ES9318 changes passes 59 cases each in the default, CX31993-disabled, ES9318-disabled, and reported-entries-disabled configurations, covering ES9318 FS/HS descriptors, the driver table, and ISO scheduling packet counts.

For detailed progress see the [plan](plan.zh.md) (Chinese), the [component API docs](components/easy-esp-uacx/README.md), and the [examples and acceptance steps](examples/README.md).

## Supported DACs

DACs with a hardware-verified driver entry (environment: ESP-IDF 5.5.1, USB Host 1.4.1):

| DAC | USB ID | Class | Board / speed | PCM | Sample rates | Kconfig | Verified |
|---|---|---|---|---|---|---|---|
| CX31993 | `06cb:1594` | UAC2 | S3 FS | 16/24 | 8, 16, 32, 44.1, 48, 96 kHz | `EUACX_DRV_CX31993` | 2026-10-05 |
| CX31993 | `06cb:1594` | UAC2 | P4 HS | 16/24/32 | 8, 16, 32, 44.1, 48, 96, 192, 384 kHz | `EUACX_DRV_CX31993` | 2026-10-05 |
| ES9318 / Savitech SA9023 | `262a:0002` | UAC1 | S3 FS | 16/24 | 32, 44.1, 48, 88.2, 96 kHz | `EUACX_DRV_ES9318` | 2026-11-22 |
| ES9318 / Savitech SA9023 | `262a:0001` | UAC2 | P4 HS | 16/24/32 | 44.1–384 kHz (8 rates) | `EUACX_DRV_ES9318` | 2026-11-22 |
| CS43198 (YUNJI43198PRO) | `2fc6:f0fc` | UAC2 | P4 HS | 16/24/32 | 44.1–384 kHz (8 rates) | `EUACX_DRV_CS43198` | 2026-11-22 |
| CS43131 ("CS43198 HiFi DSP Audio") | `3302:98c2` | UAC2 | S3 FS | 16/24/32 | 44.1 kHz only | `EUACX_DRV_CS43131` | 2026-11-22 |

Notes:

- ES9318 on P4 HS reports mute GET as always 0, so mute readback is disabled; volume readback works.
- CS43198 declares a 776-byte ISO OUT MPS at both speeds. **S3 FS adaptation failed**: every alt exceeds the S3 600-byte FIFO, and experiments that capped the MPS or copied the Windows open order streamed cleanly but stayed silent, so S3 publishes no PCM capability for it ([details](docs/device-notes.md#s3-fs-adaptation-failed)). P4 needs `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` (already set in `examples/p4/sdkconfig.defaults`).
- CS43131 also declares a 776-byte ISO OUT MPS; on S3 FS the host pipe MPS is capped at 582 bytes. Its FS firmware only plays 44.1 kHz (other rates accepted by the clock come out garbled or muted), answers GET_INTERFACE with a wrong alt, and fails mute readback, so it registers 44.1 kHz only, without mute ([details](docs/device-notes.md#330298c2-cs43131-cs43198-hifi-dsp-audio)).
- Other standard UAC2 DACs fall back to the generic driver and may work, but are not verified. Reported candidates (Edifier MF200, SMSL, iFi iDSD) have no driver entry yet; see [device notes](docs/device-notes.md).
- To add a DAC, collect its VID:PID, full descriptors and a reproduction, then follow the admission steps in the device notes.

## Quick start

Verified environment: ESP-IDF **5.5.1**, USB Host **1.4.1** (the component pins this dependency). The component requires ESP-IDF ≥5.4; other versions are not yet verified.

```sh
git clone https://github.com/easymcucourse/easy-esp-uacx.git
cd easy-esp-uacx
git submodule update --init --recursive third_party/esp-usb
cd examples/s3
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

For P4 use `examples/p4` and `esp32p4`. Connect the DAC to a Host port with VBUS power, and use a separate serial port for flashing/logs. The example first runs the Unity tests, then runs the push/pull matrix according to the DAC's capabilities.

The pull example below outputs one second of PCM silence in a format the device supports; replace `provide_pcm` with a decoder or audio-file data provider:

```c
#include <string.h>
#include "easy_uacx.h"

static size_t remaining;
static int provide_pcm(euacx_port_t *port, void *buf, size_t len, void *user)
{
    (void)port; (void)user;
    if (!remaining) return EUACX_DATA_END;
    if (len > remaining) len = remaining;
    memset(buf, 0, len);
    remaining -= len;
    return (int)len;
}

static void connected(euacx_port_t *port, const euacx_info_t *info, void *user)
{
    (void)user;
    unsigned b = info->pcm[1].num_rates ? 1 : info->pcm[0].num_rates ? 0 : 2;
    if (!info->pcm[b].num_rates) return;
    uint8_t bits = 16 + 8 * b;
    uint32_t hz = info->pcm[b].rates[0];
    remaining = hz * 2 * (bits / 8);  // stereo, one second
    euacx_stream_config_t stream = {
        .format = EUACX_FORMAT_PCM, .sample_rate = hz,
        .bits = bits, .channels = 2, .on_data = provide_pcm,
    };
    esp_err_t error = euacx_stream_open(port, &stream, NULL);
    // Handle error in your application.
    (void)error;
}

void app_main(void)
{
    euacx_config_t config = EUACX_CONFIG_DEFAULT();
    config.cb.on_connected = connected;
    ESP_ERROR_CHECK(euacx_init(&config));
}
```

In the default verified-capabilities mode, the CX31993 FS table contains the six PCM16/24 combinations that passed this round, and the HS table contains the eight PCM16/24/32 combinations (up to 384 kHz). For development testing you can disable `EUACX_USE_VERIFIED_CAPS`; capabilities read from the device are stored per input bit depth in `info.pcm[0/1/2]`, and opening a stream still sets and reads back the sample rate.

The ES9318 (Savitech SA9023, `262a:0002`) S3 FS capability table contains PCM16/24 at 32, 44.1, 48, 88.2 and 96 kHz. On the P4 HS port it enumerates as `262a:0001` in UAC2 mode, and its capability table contains PCM16/24/32 at eight rates from 44.1 to 384 kHz. It uses the same public API; see the [device notes](docs/device-notes.md#262a0002-es9318--savitech-sa9023) for details.

## Testing and configuration

```powershell
# Local ESP-IDF install under C:\Espressif; passing -Port also flashes
./scripts/idf_playback.ps1 -Board s3 -Port COM9
./scripts/idf_playback.ps1 -Board p4
# Separate config and build directories; existing sdkconfig is untouched
./scripts/idf_playback.ps1 -Board s3 -Profile Matrix -Port COM9
./scripts/idf_playback.ps1 -Board s3 -Profile Soak -Port COM9
# Host tests need no board; require MinGW GCC and the Unity bundled with ESP-IDF
./scripts/test_host.ps1
./scripts/test_host.ps1 -WithoutCx31993
```

| Option | Default | Purpose |
|---|---|---|
| `EUACX_DRV_CX31993` | y | Dedicated driver for `06cb:1594` with −10.5 dB initial volume |
| `EUACX_DRV_ES9318` | y | Dedicated driver, capability tables and 0 dB initial volume for `262a:0002` (FS) and `262a:0001` (HS) |
| `EUACX_DRV_CS43198` | y | Dedicated driver, P4 HS capability table and −10.5 dB initial volume for `2fc6:f0fc` |
| `EUACX_DRV_CS43131` | y | Dedicated driver for `3302:98c2`: S3 FS 44.1 kHz capability table, FS MPS cap 582, alt readback skipped, −10.5 dB initial volume |
| `EUACX_USE_VERIFIED_CAPS` | y | When a verified table exists, publish only the intersection of the table and the descriptors; generic uses descriptor capabilities |
| `EUACX_BUFFER_MS` | 40 | Minimum buffer duration, grown to fit the USB pipeline and rounded up to a power of two |
| `EUACX_NUM_TRANSFERS` | 4 | In-flight ISO transfers (2–8) |
| `EUACX_MAX_ERRORS` | 8 | Stop after this many consecutive ISO errors |
| `EUACX_RECOVERY_MS` | 5000 | When the component installs the Host itself, root-port enumeration retry interval while no DAC is present; 0 disables |
| `EXAMPLE_SOAK_SECONDS` | 0 | Set to 1800 to run 30 minutes of PCM24/48 kHz playback after the matrix |

See the component docs for task, queue and behavior details. Host tests cover parsing, selectors, driver tables, bit-depth conversion, feedback decoding, state permissions and the ring buffer; the USB/FreeRTOS lifecycle is verified by on-board test cases.

## Hardware verification log

2026-10-04, using the reference program `examples/common/uac2_playback_demo.c` with CX31993 (`06cb:1594`) PCM24; the user confirmed the sound was correct:

| Board | Speed | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|---|
| S3 v0.2 | FS | PASS | PASS | PASS | Insufficient bandwidth, skipped |
| P4 v1.3 | HS | PASS | PASS | PASS | PASS |

On 2026-10-05 the new driver completed S3 PCM16/24 across six configurations (24 runs) and P4 PCM16/24/32 across eight configurations (48 runs); all pull configurations had zero errors/zero underruns. All 39 P4 on-board tests passed. At the user's request, UR22C input 1/2 were recorded, which located and fixed 24/32-bit 384 kHz underruns and persistent noise caused by the example generating data byte by byte; re-recording showed no such wideband noise. The S3 long run was interrupted after about 13 min 42 s due to a board swap; the full 30-minute run and hot-plug during playback are still pending acceptance, so the release remains an RC. The old reference playback implementation has been removed; historical data is kept for comparison.

## Scope and limitations

The 1.0 scope is a single DAC with stereo PCM. There is no recording, multi-device support, resampling, or DSD-to-PCM conversion. One default Host port is currently exposed: S3 FS, P4 HS. The planned optional second FS root port on P4 does not have a backend yet, and the root controller's identity cannot be inferred from the enumeration speed.

Explicit feedback parsing and scheduling are implemented; this CX31993 FS descriptor has no feedback endpoint, so hardware verification with an asynchronous-feedback DAC has not been done yet. Complex clock selectors/multipliers fall back to the first clock source and log a warning. Hardware volume uses the first RANGE subrange; untrustworthy controls are detected and isolated during enumeration. Unsupported controls are reported via `has_volume/has_mute` and `ESP_ERR_NOT_SUPPORTED`. If the application uses an already-installed Host (`install_usb_host=false`), the application is responsible for the library event task, root-port recovery, and final uninstall.

An example `PASS` means the playback flow and stop reason matched expectations; sound quality still needs to be confirmed by listening. The `errors/underruns` in the final stop log are used for acceptance. The S3 hub uses a 100 ms reset recovery and a 500 ms power-on wait; automatic reconnection after reset still needs to be verified.

## Layout and license

| Path | Contents |
|---|---|
| `components/easy-esp-uacx/` | Public header, full PCM runtime, drivers and tests |
| `examples/common/` | Public-API playback matrix and cross-task checks |
| `examples/s3/`, `examples/p4/` | Board projects and default USB configuration |
| `scripts/` | Build/flash, host tests, serial capture |
| `third_party/esp-usb/` | The only remaining submodule, used as a USB class driver reference |

Original code is licensed under [MIT](LICENSE), copyright easymcucourse; the ESP-IDF and USB Host dependencies follow their own licenses. The design references [Averyy/esp-uac2-host](https://github.com/Averyy/esp-uac2-host) (MIT); this repository contains none of its code.

## Device behavior references

Device behavior facts are drawn from public Linux community issue reports, and the protocol implementation follows [USB-IF Audio 2.0](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Sep_14_2026.pdf); this repository contains no Linux code or device tables. For the 1.1 device-quirk implementation and acceptance progress, see the [reference plan](plan-linux-quirks.zh.md) (Chinese), the [device notes](docs/device-notes.md), and the component contribution guidelines. Host tests now include device control simulation; `./scripts/test_host.ps1 -WithoutCx31993 -WithoutReported` checks the configuration with device entries disabled.
