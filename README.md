# easy-esp-uacx
Small pluggable UAC2 Host framework for ESP32-S3 / ESP32-P4 (PCM / DoP / Native DSD).

Layers: App -> Manager -> Capability/Selector -> PCM/DoP/DSD -> Quirk -> ISO engine -> ESP USB Host.
Roadmap: Generic UAC2 PCM (S3) -> DoP -> Native DSD -> quirks -> P4 HS.

## Layout

| Path | Role |
|---|---|
| `components/easy-esp-uacx/` | **Own code** – the framework (component name `easy-esp-uacx`) |
| `third_party/esp-uac2-host/` | submodule: [Averyy/esp-uac2-host](https://github.com/Averyy/esp-uac2-host) → component `components/uac2_host` (S3 UAC2 reference driver) |
| `third_party/esp-usb/` | submodule: [espressif/esp-usb](https://github.com/espressif/esp-usb) → component `host/class/uac/usb_host_uac` (UAC1) |
| `third_party/micro-flac/` | submodule: [esphome-libs/micro-flac](https://github.com/esphome-libs/micro-flac) (FLAC decoder) |
| `examples/s3`, `examples/p4` | framework test apps |
| `examples/player` | ESP32-S3 music player (uses all of the above) |

## Clone

```
git clone --recurse-submodules <repo>
# or: git submodule update --init
# micro-flac has a nested submodule: git -C third_party/micro-flac submodule update --init
```

Examples add the needed component dirs via `EXTRA_COMPONENT_DIRS` (see `examples/player/CMakeLists.txt`).
