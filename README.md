# easy-esp-uacx

面向 ESP32-S3（FS）和 ESP32-P4（HS）的 USB Audio Class 2.0 立体声播放驱动。应用只包含 `easy_uacx.h`，即可查询 DAC 能力、推送或拉取 PCM、调节硬件音量并接收状态通知。

## 当前状态

**1.0.0-rc.1，正在进行硬件验收。** PCM 运行时已实现，示例已切换到公开 API。正式 `v1.0.0` 发布仍需完成新驱动的播放矩阵、听音、拔插恢复和 30 分钟播放；这些结果不会用历史参考程序的记录代替。

| 功能 | 实现 |
|---|---|
| 公共接口 | 初始化/释放、持久端口、状态/能力快照、连接/断开/停止回调 |
| PCM | 16/24/32 位交错小端立体声，推送/拉取，跨调用拼帧，向更高位深转换 |
| USB | UAC2 描述符、播放时钟、GET RANGE、SET/GET INTERFACE、ISO 调度、显式反馈端点 |
| 驱动 | 精确 VID:PID → 厂商通配 → generic；每次枚举建立独立设备实例 |
| 音量 | 播放通路 Feature Unit 的音量/静音控制、范围裁剪、步进对齐及读回 |
| 生命周期 | manager 串行状态变更、独立 USB/回调/供数任务、abort 唤醒、延迟关闭、枚举重试 |
| 2.0 预留 | DSD/DoP 类型已定义；1.0 能力为空，开流返回 `ESP_ERR_NOT_SUPPORTED` |

2026-10-05 验证：主机纯算法测试在启用/禁用 CX31993 两种配置下各 **33 项通过**；S3 实机 **37 项测试通过**，P4 实机 **39 项测试通过**，包括重复 init/deinit、堆恢复、实际批量写入和请求拥塞。两板跨任务权限/abort、拉取回调阻塞时的音量和关闭检查通过。S3 PCM16/24 六档共 24 次、P4 PCM16/24/32 八档共 48 次播放通过，拉取均零错误/零欠载。S3 启动爆音消失经用户确认；P4 384 kHz 杂音修复经 input 1/2 录音对照检查。完整 30 分钟仍待完成。

详细进度见 [计划](plan.zh.md)、[组件 API 文档](components/easy-esp-uacx/README.md) 和 [示例与验收步骤](examples/README.md)。

## 快速开始

验证环境：ESP-IDF **5.5.1**、USB Host **1.4.1**（组件固定此依赖）。组件要求 ESP-IDF ≥5.4；其他版本尚未验证。

```sh
git clone https://github.com/easymcucourse/easy-esp-uacx.git
cd easy-esp-uacx
git submodule update --init --recursive third_party/esp-usb
cd examples/s3
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

P4 使用 `examples/p4` 和 `esp32p4`。DAC 连接到带 VBUS 供电的 Host 口，烧录/日志使用独立串口。示例先运行 Unity 测试，再按 DAC 能力运行推送/拉取矩阵。

以下拉取示例输出 1 秒设备支持的 PCM 静音，可将 `provide_pcm` 替换为解码器或音频文件供数函数：

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

默认已验证能力模式下，CX31993 的 FS 表包含本轮通过的 PCM16/24 六档组合，HS 表包含 PCM16/24/32 八档组合（最高 384 kHz）。开发测试可关闭 `EUACX_USE_VERIFIED_CAPS`；读取的能力按输入位深分别保存在 `info.pcm[0/1/2]`，开流仍会设置并读回采样率。

## 测试与配置

```powershell
# C:\Espressif 下的本机 ESP-IDF 安装；传 -Port 时同时烧录
./scripts/idf_playback.ps1 -Board s3 -Port COM9
./scripts/idf_playback.ps1 -Board p4
# 独立配置和构建目录，不改现有 sdkconfig
./scripts/idf_playback.ps1 -Board s3 -Profile Matrix -Port COM9
./scripts/idf_playback.ps1 -Board s3 -Profile Soak -Port COM9
# 主机测试不需要板卡；依赖 MinGW GCC 和 ESP-IDF 自带 Unity
./scripts/test_host.ps1
./scripts/test_host.ps1 -WithoutCx31993
```

| 配置 | 默认 | 用途 |
|---|---|---|
| `EUACX_DRV_CX31993` | y | `06cb:1594` 专用驱动及 −10.5 dB 初始音量 |
| `EUACX_USE_VERIFIED_CAPS` | y | 有已验证表时只公布表与描述符的交集；generic 使用描述符能力 |
| `EUACX_BUFFER_MS` | 40 | 缓冲时长下限，按 USB 管线和 2 的幂扩容 |
| `EUACX_NUM_TRANSFERS` | 4 | 在途 ISO 传输数（2–8） |
| `EUACX_MAX_ERRORS` | 8 | 连续 ISO 错误达到该值时停止 |
| `EUACX_RECOVERY_MS` | 5000 | 自行安装 Host 时，无 DAC 的根端口枚举重试间隔；0 关闭 |
| `EXAMPLE_SOAK_SECONDS` | 0 | 设为 1800 开启矩阵后的 30 分钟 PCM24/48 kHz 播放 |

任务、队列和行为细节见组件文档。主机测试覆盖解析、选择器、驱动表、位深转换、反馈解码、状态权限和环形缓冲；USB/FreeRTOS 生命周期由板上用例验证。

## 硬件验证记录

2026-10-04 使用参考程序 `examples/common/uac2_playback_demo.c`，CX31993（`06cb:1594`）PCM24，用户确认声音正常：

| 板卡 | 速度 | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|---|
| S3 v0.2 | FS | PASS | PASS | PASS | 带宽不足，跳过 |
| P4 v1.3 | HS | PASS | PASS | PASS | PASS |

新驱动于 2026-10-05 完成 S3 PCM16/24 六档共 24 次播放，以及 P4 PCM16/24/32 八档共 48 次播放；所有拉取档位零错误/零欠载。P4 39 项板上测试通过。按用户要求录制 UR22C input 1/2，定位并修复示例逐字节生成造成的 24/32 位 384 kHz 欠载和持续杂音，复测录音无该宽带噪声。S3 长播约 13 分 42 秒后因换板中断；完整 30 分钟和播放中热插拔仍待验收，当前保留 RC 版本。旧参考播放实现已移除，历史数据保留供对照。

## 范围与限制

1.0 范围是单 DAC、立体声 PCM。无录音、多设备、重采样或 DSD 转 PCM。当前公开一个默认 Host 端口：S3 FS、P4 HS；计划中的 P4 可选第二个 FS 根端口尚未提供后端，不能用枚举速度推断根控制器身份。

显式反馈解析及调度已实现；CX31993 这份 FS 描述符没有反馈端点，尚未以异步反馈 DAC 做硬件验证。复杂时钟选择器/倍频器使用首个时钟源回退并记录警告。硬件音量使用首个 RANGE 子区间，枚举时检测并隔离不可信控制；未支持的控制通过 `has_volume/has_mute` 和 `ESP_ERR_NOT_SUPPORTED` 表达。若应用使用已安装的 Host（`install_usb_host=false`），应用负责库事件任务、根端口恢复及最终卸载。

示例 `PASS` 表示播放流程及停止原因符合预期，声音质量仍需听音确认；最终停止日志中的 `errors/underruns` 用于验收。S3 Hub 使用 100 ms 复位恢复和 500 ms 上电等待；目前仍需验证复位后自动重连。

## 目录与许可证

| 路径 | 内容 |
|---|---|
| `components/easy-esp-uacx/` | 公共头文件、完整 PCM 运行时、驱动及测试 |
| `examples/common/` | 公共 API 播放矩阵和跨任务检查；历史参考程序 |
| `examples/s3/`、`examples/p4/` | 板型工程和默认 USB 配置 |
| `scripts/` | 编译烧录、主机测试、串口采集 |
| `third_party/esp-usb/` | 唯一保留的子模块，USB 类驱动参考 |

自有代码采用 [MIT](LICENSE)，版权 easymcucourse；ESP-IDF 和 USB Host 依赖遵循各自许可证。设计参考 [Averyy/esp-uac2-host](https://github.com/Averyy/esp-uac2-host)（MIT），本仓库不包含其代码。

## 设备行为参考

设备行为事实参考 Linux 社区公开问题报告，协议实现依据 [USB-IF Audio 2.0](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Sep_14_2026.pdf)；本仓库不包含 Linux 代码或设备表。1.1 设备特例软件实现及验收进度见 [参考计划](plan-linux-quirks.zh.md)、[设备记录](docs/device-notes.md) 和组件贡献须知。主机测试现包含设备控制模拟；`./scripts/test_host.ps1 -WithoutCx31993 -WithoutReported` 检查关闭设备条目的配置。
