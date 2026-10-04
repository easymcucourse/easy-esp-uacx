# ESP32-S3 SDSPI + I2S/CX31993 音乐播放器

当前工程面向 ESP32-S3-WROOM-1-N16R8。它从 SD 卡扫描 MP3、PCM WAV 和 FLAC，
软件解码为 PCM。检测到支持的 USB 声卡时优先通过 USB 播放；USB 不可用、流启动失败或设备
断开时自动回退到板载 I2S 声卡。支持 Synaptics CX31993（UAC2）和 Creative Sound
Blaster PLAY! 3（UAC1，VID:PID `041E:324D`）。

## 功能概览

- MP3、PCM WAV、FLAC 递归扫描与循环播放
- ST7789 状态显示和音频频谱
- I2S、UAC1、UAC2 音频输出与 USB 热插拔回退
- 串口按键控制、Web 控制页和局域网 TCP OTA
- 16 MB Flash、双 OTA 分区和 PSRAM 音频缓冲

## 环境与依赖

- ESP-IDF 5.5.1
- ESP32-S3-WROOM-1-N16R8
- FAT32 SD 卡
- UAC2 Host、microFLAC 和 Espressif UAC1 Host 源码

默认从项目的相邻目录读取外部组件：

```text
<workspace>/
├── esp32_music_player/
├── esp32_CX31993/components/uac2_host/
├── esp32base/L032_flac_usb_audio/managed_components/esphome__micro-flac/
└── esp-uac2-host-play3/ref/espressif-uac1/
```

也可以分别设置 `UAC2_HOST_DIR`、`MICRO_FLAC_DIR` 和 `UAC1_HOST_DIR` 环境变量，
指向这三个依赖目录。

## 快速开始

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p <串口> flash monitor
```

在 `menuconfig` 的 `Music Player` 菜单中设置 Wi-Fi。留空时，播放器仍可离线运行，
但不会启动 Web 控制和 TCP OTA。将音乐文件复制到 FAT32 SD 卡后，固件会递归扫描并播放。

## ST7789 显示屏

使用原生 240×320 ST7789、RGB565 和硬件 SPI3。显示屏左转 90°，逻辑分辨率为
320×240。显示屏在 SD 卡测试之前初始化，并显示
启动、FAT32 挂载、音乐扫描和播放状态。播放页顶部显示硬件信息、Wi-Fi IP
和当前实际播放声卡，中间显示当前文件名，底部显示 Visualizer 条形区。SD 卡单独使用 SPI2，因此两个设备
不会重复初始化同一个 SPI Host。

| ST7789 信号 | ESP32-S3 |
| --- | ---: |
| SCLK | GPIO42 |
| MOSI / SDA | GPIO41 |
| RST / RES | GPIO40 |
| DC / A0 | GPIO39 |
| CS | GPIO38 |

背光引脚未由软件控制；模块的 `BL`、`LED` 或 `BLK` 应按模块规格接到 3.3V。

## 音频输出

I2S 声卡使用标准 Philips I2S、32-bit 双声道，支持随文件在 8–96 kHz 之间动态切换；
MCLK 固定为采样率的 256 倍。内部采用 Q1.31 音频和千分比音量运算，24/32-bit PCM WAV
不会再预先截断到 16-bit；16-bit MP3 解码结果也会在调节音量前扩展到 Q1.31。

FLAC 使用 `L032_flac_usb_audio` 中的 microFLAC 0.1.1 流式解码器。支持原生
`.flac`、单声道或双声道、8–32 bit PCM；解码器输出保持为左对齐 Q1.31，24-bit
文件不会先降为 16-bit。解码工作缓冲优先放入 PSRAM，并启用 ESP32-S3 Xtensa
LPC 优化。FLAC 文件输入缓存为 64KB，低于 16KB 时提前从 SD 卡补充，减小 SD
读抖动造成的爆音。FLAC 解码任务会提前把 PCM 写入 768KB PSRAM ring buffer，
播放任务从 ring 中稳定读取并写声卡。采样率直接采用文件原始值，不进行重采样；
当前音频输出范围为 8–96 kHz。

| 声卡信号 | ESP32-S3 | 方向/用途 |
| --- | ---: | --- |
| WS | GPIO9 | 左右声道时钟 |
| DIN | GPIO10 | ESP32 数据输出到声卡 |
| BCK | GPIO11 | 位时钟 |
| MC / MCLK | GPIO12 | 主时钟，256fs |
| SD | GPIO13 | 声卡使能，当前配置为高有效 |

默认音乐音量为 17.5%，是原 35% 幅度的一半；每次音量按键仍增减 5.0%。启动时会播放
约 250 ms 的测试音，其幅度也已减半。若声卡的 `SD` 实际为低有效，可在
`main/board_pins.h` 中把 `MUSIC_I2S_SD_ENABLE_LEVEL` 改为 `0`。

播放器不会等待 USB 设备；无 USB 时立即使用 I2S。运行中发现可用 UAC2 TX 接口后，
动态打开设备并按当前曲目采样率启动音频流，USB 成功启流后自动成为首选输出：

- USB Host：原生 `D-=GPIO19`、`D+=GPIO20`
- 目标设备：CX31993，VID:PID `06CB:1594`
- UAC：USB Audio Class 2.0
- USB 播放流：优先 24-bit 立体声，不支持时尝试 16-bit。CX31993 的同步 OUT 端点
  原生支持 44.1 kHz，驱动使用 44/45 帧的分数包调度，不进行采样率转换
- 热插拔：发现 UAC2 TX 接口后创建设备任务；CX31993 会额外应用静音和音量控制，
  其他 UAC2 播放设备也可尝试启流；拔出后自动停止、关闭并恢复 I2S
- USB 配置：控制传输 2048 字节、周期 OUT FIFO、4 个等时 URB、每 URB 1 包

UAC2 驱动默认来自相邻参考工程：

```text
../esp32_CX31993/components/uac2_host
```

Sound Blaster PLAY! 3 使用参考工程中的 UAC1 类驱动，按曲目原生采样率自动选择
24-bit 或 16-bit 播放格式，不执行采样率转换：

```text
../esp-uac2-host-play3/ref/espressif-uac1
```

`main/CMakeLists.txt` 编译该参考目录中的 UAC1 源码，并加入 PLAY! 3 专用的
VID/PID 过滤、动态启流和热插拔管理。

## SDSPI 接线

使用 SPI2 Host，接线如下：

| SD 信号 | 原接口标识 | ESP32-S3 |
| --- | --- | ---: |
| CS | CS | GPIO15 |
| MOSI | MOSI | GPIO16 |
| CLK | CLK | GPIO17 |
| MISO | MISO | GPIO18 |

SD 卡 SPI 协议规定使用 Mode 0（CPOL=0、CPHA=0）。启动时不再执行多档速度扫描，
直接以固定 20 MHz 初始化并挂载 SD 卡；挂载失败时每 3 秒仍以 20 MHz 重试。

### 实机测试记录

2026-08-29 在测试板上验证：CX31993 枚举、描述符解析、静音和左右音量控制全部
通过，并打印 `CX31993 DRIVER LOAD PASS`。SDSPI 实测结果：

| 请求/实际频率 | 64 KiB 重复读速度 | 结果 |
| ---: | ---: | --- |
| 0.4 MHz | 46 KiB/s | PASS，兼容字节寻址 |
| 1 MHz | 112 KiB/s | PASS |
| 4 MHz | 389 KiB/s | PASS |
| 10 MHz | 765 KiB/s | PASS |
| 20 MHz | 1133 KiB/s | PASS，运行档位 |
| 40 MHz | — | High Speed 初始化返回 `ESP_ERR_INVALID_RESPONSE` |

GPIO15–18 在 20 MHz 下读取稳定。当前 1864 MiB 卡存在非标准行为：OCR/CSD 报告
SDHC 块寻址，但实际 CMD17/CMD24 参数仍按字节地址解释。Windows 已验证该卡的 MBR
分区从 LBA 4096 开始，FAT32 BPB 和 `55AA` 签名完整，`chkdsk` 无错误。

固件会先用标准 SDHC 寻址并严格验证 FAT BPB；若失败，再以 `LBA × 512` 字节寻址
重读。只有兼容读取获得有效 BPB 时才切换，并让 FatFs 后续读写沿用相同模式。该检测
不会格式化卡片。卡内可放置 `.mp3` 或 PCM `.wav`，程序会递归扫描并循环播放。

兼容固件实测打印 `SD ADDRESS COMPAT PASS`，各通过档位读取 FAT32 卷数据的校验和
均为 `d97a744b`。20 MHz 挂载后容量为 1858 MiB、空闲 1854 MiB，扫描到
`EGOIST - Euterpe.mp3`，并由 I2S 以 44.1 kHz、32-bit stereo 播放；CX31993 仅完成
驱动加载，USB Streaming 接口保持 IDLE。

## 串口控制

调试串口为 `115200 8N1`：

| 按键 | 功能 |
| --- | --- |
| `P` 或空格 | 播放/暂停 |
| `N` | 下一曲 |
| `B` | 上一曲 |
| `R` | 重播 |
| `+` / `-` | 音量增减 5% |
| `S` | 状态 |
| `L` | 曲目列表 |
| `H` 或 `?` | 帮助 |

## Wi-Fi 与 TCP OTA

Wi-Fi 凭据不保存在仓库中。执行 `idf.py menuconfig`，在 `Music Player` 菜单中设置
SSID 和密码；配置写入已被 Git 忽略的本地 `sdkconfig`。SSID 留空时禁用 Wi-Fi、
Web 控制和 TCP OTA。启用后，设备以 Station 模式连接并在 TCP `3333` 端口等待 OTA，
串口日志会打印 DHCP 分配的 IPv4 地址。

编译后执行：

```powershell
python tcp_ota.py <设备IP> build\esp32_s3_music_player.bin
```

成功时客户端显示 `OTA READY` 和 `OTA OK REBOOTING`，设备切换到另一个应用分区并
自动重启。此端口只更新应用程序，不能更新 Bootloader 或分区表。它没有 OTA 身份验证
或 TLS，仅应在可信、隔离的局域网使用，不能暴露到互联网。

## 编译

```powershell
idf.py build
```

完整烧录、OTA 限制和故障排查见 [docs/FLASHING.md](docs/FLASHING.md)。

## 项目结构

```text
.
├── main/                 # 播放器、显示、存储、网络和音频输出源码
├── docs/FLASHING.md      # 完整烧录与 OTA 说明
├── CMakeLists.txt        # ESP-IDF 工程及外部组件路径
├── partitions.csv       # 16 MB 双 OTA 分区表
├── sdkconfig.defaults   # 可公开的默认构建配置
└── tcp_ota.py           # TCP OTA 上传客户端
```
