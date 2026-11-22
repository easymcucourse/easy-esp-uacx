# Examples

English | [中文](README.zh.md)

Both projects share `common/easy_uacx_demo.c` and `common/easy_uacx_validation.c` and use the component through its single public header `easy_uacx.h`. After the new driver completed the two-board matrix, the old reference playback implementation was removed; historical acceptance data is kept in this document.

## Wiring and building

| Project | DAC connection | Flash/log |
|---|---|---|
| `s3` | FS host: D− GPIO19 / D+ GPIO20, with VBUS power | UART0; tested here on COM9 (CH343) |
| `p4` | Host port on the default internal HS PHY, with VBUS power | USB Serial/JTAG; historically COM10 |

The actual USB speed is determined by device enumeration; an FS DAC on the P4 still runs at FS. Only one default host port is currently exposed; the optional second P4 FS root port is not included.

```sh
cd examples/s3
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

For P4 use `examples/p4` and `esp32p4`. On Windows with ESP-IDF installed at `C:\Espressif`, run from the repository root:

```powershell
./scripts/idf_playback.ps1 -Board s3 -Port COM9
./scripts/idf_playback.ps1 -Board p4
# Full bit-depth matrix of descriptor capabilities (one bar per rate)
./scripts/idf_playback.ps1 -Board s3 -Profile Matrix -Port COM9
# Verified matrix + 30-minute PCM24/48k + resource-release check
./scripts/idf_playback.ps1 -Board s3 -Profile Soak -Port COM9
```

Without `-Port` the script only builds. Matrix/Soak configs and artifacts live in the project's `build/matrix/` and `build/soak/` respectively and leave the existing `sdkconfig` untouched; their default configs are `common/sdkconfig.matrix.defaults` and `common/sdkconfig.soak.defaults`. An existing profile config can be adjusted with menuconfig in that build directory.

After closing any other monitor holding the serial port, capture logs with:

```powershell
C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe scripts/capture_serial.py --port COM9 --seconds 120 --reset --output examples/s3/matrix_serial.log
# 30-minute playback after the matrix, leaving time for startup and teardown
C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe scripts/capture_serial.py --port COM9 --seconds 1900 --reset --output examples/s3/soak_serial.log
# Strictly check the UART record; exits non-zero on missing or failed items
python scripts/check_validation.py examples/s3/matrix_serial.log --emit-caps
python scripts/check_validation.py examples/s3/soak_serial.log --require-soak
```

On Windows you can record two channels by input device name while resetting the board and capturing its log at the same time:

```powershell
python -m pip install sounddevice soundfile
python scripts/record_input.py --input 'Input 1/2' --seconds 105 --output tmp/p4-audio/matrix.wav --serial COM10 --serial-python C:/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe
```

By default it uses the input interface's current sample rate (44.1 kHz for the local UR22C) and does not open a monitor output. The WAV, serial log, and a JSON with start time and dropped-frame count are saved under the same base name; dropped recording frames return a non-zero status, and such a recording cannot be used to judge playback quality.

## Current test flow

1. Run the on-board Unity tests, covering the existing private algorithms, driver tables, the real CX31993 descriptor, conversion/feedback/RANGE, state permissions, counter wraparound, 3 USB Host init/release cycles with heap recovery, real write frame assembly/batch conversion/timeout/abort, deinit inside a status callback, callback-queue overflow ordering and completeness, and 8 tasks concurrently submitting 128 requests.
2. Wait for the DAC and print `conn_id`, VID:PID, FS/HS, driver, per-input-bit-depth capabilities, and volume range. Stream open sets the device-side playback alt and sample rate. CX31993 uses an initial volume of −10.5 dB, ES9318 uses 0 dB.
3. Cross-task checks: an external task's write/close on a push stream is rejected, and abort wakes the owning task; while a pull callback blocks, an external task can still query volume, and close completes after the callback exits. Prints `VALIDATION`.
4. For every bit depth and rate in `info.pcm[0/1/2]`, play "Twinkle, Twinkle, Little Star" in pull and push mode. Data is deliberately fed in non-frame-aligned 4093-byte chunks. Pull drains on EOF; push keeps the tail playing for a while before close.
5. Print `RESULT`, `MATRIX`, `VERIFIED_RATE`, `SUMMARY`. Then run `SOAK` per the configuration, printing `STATS` every 30 seconds; at the end of the first round, a lifecycle/heap check with re-initialization can run.

`RESULT PASS` means stream open, feeding, connection, and stop reason were as expected; you still need to confirm the actual sound and the error/underrun counts in the final `stream stopped`. Silence sent while waiting for the push tail may count as underruns; 30-minute pull acceptance requires `errors=0 underruns=0` throughout, and EOF silence padding does not count as underruns.

`VERIFIED_RATE` lines are candidates for the registered capability table and must be combined with listening or recording checks and the actual alt/subslot in the log; descriptor capabilities must not be written directly into a verified table. The Matrix config disables verified filtering; the Soak config enables it. The CX31993 FS table registers this round's six rates for PCM16/24; the HS table registers eight rates each for PCM16/24/32 (8/16/32/44.1/48/96/192/384 kHz). The S3 PCM32 endpoint's MPS 768 exceeds the current FIFO of 600 and is correctly excluded from capabilities.

Matrix coverage of the registered dedicated drivers (push + pull per rate; check each log with `check_validation.py` above and confirm the VID:PID and driver name in the serial output):

| DAC | Board / USB ID | Matrix | Notes |
|---|---|---|---|
| ES9318 | S3 FS `262a:0002` | PCM16/24 × 32, 44.1, 48, 88.2, 96 kHz = 20 | UAC1; [notes](../docs/device-notes.md#262a0002-es9318--savitech-sa9023) |
| ES9318 | P4 HS `262a:0001` | PCM16/24/32 × 44.1–384 kHz = 48 | |
| CS43198 | P4 HS `2fc6:f0fc` | PCM16/24/32 × 44.1–384 kHz = 48 | Needs the periodic-OUT FIFO bias (set in `p4/sdkconfig.defaults`); S3 FS failed, no PCM capability ([notes](../docs/device-notes.md#s3-fs-adaptation-failed)) |
| CS43131 | S3 FS `3302:98c2` | PCM16/24/32 × 44.1 kHz = 6 | 48–96 kHz transfer fine but the recorded output is garbled or muted ([notes](../docs/device-notes.md#330298c2-cs43131-cs43198-hifi-dsp-audio)) |
| CS43131 | P4 HS `3302:98c2` | PCM16/24/32 × 44.1–384 kHz = 48 | Needs the periodic-OUT FIFO bias |
| ES9039Q2M | P4 HS `2fc6:f800` | PCM16/24/32 × 44.1–384 kHz = 48 | Needs the periodic-OUT FIFO bias; S3 FS failed, no PCM capability ([notes](../docs/device-notes.md#2fc6f800-es9039q2m-sxw-mdl7601-intclk_a2)) |

`RESULT PASS` does not prove audibility. New entries were also checked by recording the DAC output with a microphone while the matrix ran, splitting the recording per stream by the serial log, and comparing tone level, broadband noise and melody pitch against the silence before playback.

## Configuration

| Option | Default / purpose |
|---|---|
| `EXAMPLE_PLAYBACK_TEST` | y; when off, only the Unity tests run |
| `EXAMPLE_PLAYBACK_BARS` | 4; 1 in the Matrix/Soak profiles |
| `EXAMPLE_SOAK_SECONDS` | 0; 1800 in the Soak profile |
| `EXAMPLE_LIFECYCLE_TEST` | y; n in the Matrix profile; does not affect the preceding Unity lifecycle tests |
| `EUACX_USE_VERIFIED_CAPS` | y; n in Matrix to iterate descriptor capabilities |
| `EUACX_NUM_TRANSFERS` | 4; range 2–8 |
| `ESP_MAIN_TASK_STACK_SIZE` | 16384; for on-board tests and example buffers |
| `USB_HOST_CONTROL_TRANSFER_MAX_SIZE` | 4096; multi-alt descriptors |
| `ESP_CONSOLE_USB_SERIAL_JTAG` | P4 primary console |
| `USB_HOST_HUBS_SUPPORTED` | External hub enabled on S3 |
| `USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT` | S3 periodic OUT FIFO, supporting CX31993's larger MPS |
| `USB_HOST_EXT_PORT_RESET_RECOVERY_DELAY_MS` | 100 ms on S3 |
| `USB_HOST_EXT_PORT_CUSTOM_POWER_ON_DELAY_MS` | 500 ms on S3; enables the custom wait |

Other task, queue, buffer, and retry options are described in the [component docs](../components/easy-esp-uacx/README.md).

## New driver acceptance record (2026-10-05)

Environment: ESP-IDF 5.5.1 / USB Host 1.4.1, 1.0.0-rc.1.

| Item | Result |
|---|---|
| S3 / P4 build | Both pass |
| Host algorithm tests | CX31993 enabled/disabled, 33 tests each pass |
| S3 on-board Unity | 37 pass, 0 fail |
| S3 CX31993 enumeration | FS succeeds; the real 422-byte descriptor is now a test fixture |
| S3 push owner task/abort | Pass; a blocked write was woken by another task |
| S3 pull blocking and deferred close | Pass; a volume request from another task took 0 ms while waiting for close |
| S3 new-driver matrix/listening | After the fix, PCM16/24 six rates × push/pull, 24 cases pass, pull zero errors/zero underruns; user confirmed the startup pop is gone |
| 30 minutes zero errors/zero underruns | S3 ran about 13 min 42 s with zero errors/zero underruns, interrupted when switching to P4; full 30 minutes still pending |
| Hub reset and auto-recovery after replug | Full acceptance pending |
| P4 new driver on hardware | 39 Unity tests and cross-task checks pass; PCM16/24/32 eight rates each × push/pull, 48 cases pass, all pull zero errors/zero underruns |
| P4 noise investigation and recording retest | UR22C input 1/2, 44.1 kHz stereo capture, no dropped recording frames; generating music byte-by-byte caused underruns at 24/32-bit 384 kHz, switching to frame-by-frame eliminated the continuous broadband noise |
| Async-feedback DAC on hardware | Pending; the current CX31993 FS descriptor has no feedback endpoint |

The repeated-init test fixed a bug where unconsumed USB library pending event flags caused uninstall to fail; after the fix, heap recovery and re-init both pass. The fast lifecycle case closes the root port before enumeration finishes, so the SDK may log enumeration-cancel messages; the final test result should be PASS with no memory loss.

The official release still requires: capability-based matrix with listening on both boards, unplug during playback verifying stopped/disconnected order and `conn_id` increment, hub reset recovery, 30-minute pull with no errors/underruns, and repeated deinit with device and stream present without leaks. 1.0 is not marked complete until these pass.

## Hardware test results (2026-10-04)

Environment: ESP-IDF 5.5.1 / USB Host 1.4.1, CX31993 DAC `06cb:1594`. Both boards use clock source 9, interface 1, alt2, 24-bit stereo packed PCM; Feature Unit 2 master mute reads back OFF, and left/right volume both read back −10.5 dB.

### P4: HS, 8000 intervals/s

ESP32-P4 v1.3, COM10. 20 unit tests pass; user confirmed normal sound.

| Sample rate | PCM bit rate | Packets completed | Bytes completed | Errors | Result |
|---|---|---|---|---|---|
| 44.1 kHz | 2.1168 Mbit/s | 39200 | 1296540 | 0 | PASS |
| 48 kHz | 2.304 Mbit/s | 39200 | 1411200 | 0 | PASS |
| 96 kHz | 4.608 Mbit/s | 39200 | 2822400 | 0 | PASS |
| 192 kHz | 9.216 Mbit/s | 39200 | 5644800 | 0 | PASS |

Each rate took 4900–4901 ms. Summary: `passed=4 skipped=0 failed=0`.

### S3: FS, 1000 intervals/s

ESP32-S3 v0.2, COM9 (CH343 UART). 20 unit tests pass; user confirmed normal sound.

| Sample rate | Packets completed | Bytes completed | Errors | Result |
|---|---|---|---|---|
| 44.1 kHz | 4904 | 1297596 | 0 | PASS |
| 48 kHz | 4904 | 1412352 | 0 | PASS |
| 96 kHz | 4904 | 2824704 | 0 | PASS |
| 192 kHz | — | — | — | SKIP: insufficient DAC FS endpoint bandwidth |

Each rate took 4914–4922 ms. Summary: `passed=3 skipped=1 failed=0`.

With a hub, Port2 / CHECK_ADDR enumeration failed after reset; after replugging the DAC it enumerated and completed all three rates. Automatic hub recovery at startup is not yet verified.

Local serial logs are `p4/playback_set_interface_serial.log` and `s3/playback_fifo_serial.log`; log files are excluded by `.gitignore` and not committed.

## Troubleshooting

| Symptom | What to check |
|---|---|
| ISO transfers PASS but no sound | A device-side `SET_INTERFACE` must be sent to enable the playback alt; also check mute and left/right volume readback |
| Descriptor too large, enumeration fails | Check that the control transfer limit is 4096 |
| S3 only sees the hub | Enable external hub support; if Port2 enumeration fails, replug the DAC |
| S3 claim of alt2 fails, MPS 576 exceeds 128 | Use the `PERIODIC_OUT` FIFO configuration |
| S3 skips 192 kHz | The current DAC's FS alternate settings lack bandwidth; expected |

## P4 regression for the Linux device-behavior plan (2026-10-06)

P4 v1.3 / COM10 flashed with the current Matrix config, ESP-IDF 5.5.1 / USB Host 1.4.1, CX31993 `06cb:1594`, HS. Flash hash verification passed.

All 39 on-board Unity tests, push owner/abort/volume, and blocking pull close/volume checks passed. Volume and mute capabilities are normal, range [−18944, 0]/128, with no enumeration readback failures; volume requests took 0 ms. With verified-capability filtering off, PCM16/24/32 × 8/16/32/44.1/48/96/192/384 kHz × push/pull, 48 runs in total, all passed; all ISO errors 0 and all pull underruns 0. The intentional silent wait at the push tail increases the underrun count. The strict log check passed and the user confirmed normal sound during playback.

The raw log is kept locally in the ignored `tmp/linux-p4-runtime.log`; no physical hot-plug or 30-minute continuous playback was done. The new quirk flags pass host simulation tests; this hardware run only verified the existing CX31993 default behavior with flags=0. S3 regression and new DAC acceptance are still pending.

## S3 current implementation regression (2026-10-06, incomplete)

S3 v0.2 / CH343 COM9 flashed with Matrix firmware, with flash hash verification passing. The 39 on-board tests and cross-task volume/abort/close checks passed, and CX31993 FS volume/mute probes are normal. 22 push/pull PASS runs were recorded for the six PCM16 rates and the first five PCM24 rates, with ISO errors 0 and pull underruns 0 for completed runs.

At the start of PCM24/96 kHz the serial connection dropped and COM9 disappeared from the system; the last two runs and the summary have no results yet and need to be completed after reconnecting the S3 flashing USB. Listening is not yet confirmed. Logs are kept locally in the ignored `tmp/linux-s3-runtime.log` and `tmp/linux-s3-capture.log`. This does not count as a full matrix pass.
