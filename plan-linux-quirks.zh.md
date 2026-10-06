---
name: 参考 Linux snd-usb-audio 的设备特例处理（保持 MIT，禁止 GPL 代码）
overview: 参考 Linux snd-usb-audio 对杂牌 USB DAC 的处理经验，为 easy-esp-uacx 增加通用健壮性检查和按 USB ID 启用的设备特例标志，覆盖静音 / 音量、采样率与 alt 切换、原生 DSD 三类问题。Linux 只作为"设备行为"的信息来源：不复制、不翻译、不改写其代码和表格，所有实现由本项目按 USB Audio 规范独立编写，保持 MIT 许可。通用检查和控制、流相关标志计划在 1.1；DSD 相关标志并入 2.0；不支持 UAC1。
todos:
  - id: l0-rules
    content: L0：写入许可证规则（第 2 节），新增 scripts/check_license.ps1 并接入主机测试脚本，新建 docs/device-notes.md
    status: completed
  - id: l1-driver-flags
    content: L1：euacx_driver_t 增加 flags 和 params，driver_validate 校验参数，单元测试覆盖
    status: completed
  - id: l1-control
    content: L1：音量 / 静音通用检查（res 为 0、范围无效、读回失败检测，通道选择保持现有规则）和控制类标志
    status: in_progress
  - id: l2-stream
    content: L2：采样率 / alt 切换类标志、同格式 alt 按最小 MPS 选择、AS_VAL_ALT_SETTINGS 校验
    status: in_progress
  - id: l3-entries
    content: L3：整理候选设备清单（只记录 VID:PID、现象、来源链接），按证据等级加入驱动表
    status: in_progress
  - id: l4-dsd
    content: L4（并入 2.0）：原生 DSD 的 alt 识别、字节序 / 位序、bcdDevice 匹配、模式切换钩子
    status: pending
isProject: false
---

# 参考 Linux snd-usb-audio 的设备特例处理

## 1. 目标与范围
- **目标**：让杂牌 USB DAC 在描述符和实际行为不一致时仍能正常播放和调音量。做法分两层：
  - **通用检查**：不依赖设备表，能从设备响应中检测出来的问题自动处理（如音量步进为 0、写入后读回不变）。
  - **设备特例标志**：检测不出来、只能按 VID:PID 指定的问题（如必须先切 alt 再设采样率），写在驱动条目里。
- **范围**：只支持 UAC2 播放（与主计划一致）。覆盖静音 / 音量、采样率与 alt 切换、原生 DSD 三类。UAC1 设备、录音、MIDI、厂商私有混音器面板不在范围内。
- **版本**：
  - L0–L3 计划在 1.1，不阻塞 1.0 发布。1.0 已通过的 CX31993 行为不能改变。
  - L4 并入主计划的 2.0（DSD）。
- **原则**：先做通用检查，设备表只补检测不出来的情况。设备表不追求"收录得多"，每个条目都要有出处（第 6 节）。

## 2. 许可证规则
本项目为 MIT。Linux `sound/usb/`（`quirks.c`、`quirks-table.h`、`mixer.c`、`mixer_quirks.c`、`format.c`、`pcm.c` 等）为 GPL-2.0，**不得以任何形式进入本仓库**。

### 2.1 可以使用的信息
| 来源 | 用法 |
|---|---|
| USB-IF 规范：Audio Device Class 2.0、Audio Data Formats 2.0、USB 2.0 | 协议实现的唯一依据 |
| 微软文档 [USB Audio 2.0 Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers) | 驱动行为参考（如同格式 alt 选最小 MPS） |
| 芯片数据手册、厂商公开文档（如 XMOS lib_xua 的 DSD 说明、CM108B 手册） | 设备行为、参数范围 |
| Linux 补丁的**提交说明**、邮件列表中的问题报告 | 只提取事实：哪个 VID:PID、什么现象、什么条件下出现 |
| 本项目自己的实机测试、USB 抓包 | 首选证据 |

只记录事实，不记录实现：例如"`2d99:a024` 的音量 `SET_CUR` 生效，`GET_CUR` 始终返回同一个值"可以记录；Linux 用什么函数、什么结构、什么判断顺序来处理，不记录、不模仿。

### 2.2 禁止的做法
- 复制、翻译（转成其他语言或改变量名）、逐段改写 Linux 的代码、宏、结构体、表格。
- 把 Linux 的设备表整段转换格式后导入；设备条目只能逐条按第 6 节流程加入。
- 包含任何 GPL 头文件（如 `linux/usb/audio.h`），或以子模块、第三方目录引入 GPL 代码。
- 使用 Linux 的内部标识符作为本项目的名称（`QUIRK_FLAG_*`、`snd_usb_*`、`USB_MIXER_*` 等），避免在代码层面形成对应关系。本项目的名称按自己的语义命名（第 4 节）。
- 实现时同时打开 Linux 源码对照编写。需要了解某个设备时，先把事实写进 `docs/device-notes.md`，再只看自己的记录和规范实现。

### 2.3 其他许可证
- OpenBSD `uaudio.c`（ISC）、FreeBSD（BSD-2-Clause）为宽松许可证，可以阅读参考；如果复制了其中的代码片段，必须在对应文件中保留原版权声明，并在 `docs/device-notes.md` 记录。默认仍按自己编写处理。
- Thesycon、XMOS 的驱动和固件为专有软件，只参考公开文档。

### 2.4 检查
- 新增 `scripts/check_license.ps1`，由 `scripts/test_host.ps1` 调用，任一项命中即失败：
  - 文件中出现 `SPDX-License-Identifier: GPL`、`GPL-2.0`、`LGPL`；
  - 出现 `QUIRK_FLAG_`、`snd_usb_`、`USB_MIXER_`、`SNDRV_` 等 Linux 标识符；
  - `#include` 了 `linux/` 下的头文件。
- 检查范围：`components/`、`examples/`、`scripts/`；排除 `third_party/esp-usb`（Apache-2.0）和本计划等文档文件。
- 所有新文件按主计划第 10 节加 MIT SPDX 文件头。

## 3. 现状与差距
对照当前代码（`core/euacx_parser.c`、`core/euacx_control.c`、`core/euacx_caps.c`、`stream/euacx_stream.c`、`private/euacx_driver.h`）：

| 项目 | 现状 | 差距 |
|---|---|---|
| Feature Unit 控制位 | 已区分只读和可读写（`(controls & 3) == 3`） | 无 |
| 音量范围 | 只读第一个可用通道的 `GET RANGE`，只接受 1 个子区间；`min > max` 或 `res < 0` 时禁用 | `res == 0` 未修正；`min == max` 未处理 |
| 音量 / 静音写入 | 每次写入后读回，不一致返回 `ESP_ERR_INVALID_RESPONSE` | `GET_CUR` 坏掉但 `SET_CUR` 有效的设备会被判定为失败；没有枚举时检测，也没有缓存写入值的回退 |
| 通道选择 | 在 master 和通道 1、2 中取第一个可读写的 | 保持不变（第 5.1 节） |
| 控制传输节奏 | 连续发送 | 不支持按设备加延时 |
| 采样率 | alt 0 → `SET_CUR` → `GET_CUR` 必须相等 → 时钟有效性 → alt N | 顺序固定；读回不等直接失败；不支持按设备跳过读回或调换顺序；`SET_INTERFACE` 后不等待 |
| alt 选择 | 位深最小，其次 subslot 最小 | 同格式多个 alt 时不比较 MPS；不检查 `AS_VAL_ALT_SETTINGS` |
| 驱动条目 | `euacx_driver_t` 有钩子和已实测能力，没有标志位 | 需要 `flags` 和参数 |
| 协议版本 | 只识别 UAC2（协议 `0x20`） | 保持不变，不支持 UAC1（第 8 节） |
| 原生 DSD | 旧模块只有"VID 白名单 + RAW_DATA" | alt 编号、字节序、位序、固件版本、模式切换都缺（L4） |

## 4. 驱动条目扩展
在 `private/euacx_driver.h` 中增加标志位和参数。名称是本项目自己的语义命名：

```c
typedef struct {
    uint16_t ctl_delay_us;       /* after every class control request */
    uint16_t iface_delay_ms;     /* after SET_INTERFACE to a streaming alt */
    int16_t  vol_min, vol_max, vol_res;  /* 1/256 dB, used with EUACX_DRV_VOL_RANGE */
    uint8_t  dsd_alt;            /* 2.0: alt carrying native DSD, 0 = detect from RAW_DATA */
    uint16_t bcd_min, bcd_max;   /* bcdDevice match range, 0/0 = any */
} euacx_driver_params_t;

typedef struct euacx_driver {
    /* existing fields ... */
    uint32_t flags;                        /* EUACX_DRV_* */
    const euacx_driver_params_t *params;   /* NULL = all defaults */
    /* existing hooks ...; 2.0 adds select_mode */
} euacx_driver_t;
```

### 4.1 标志位
| 标志 | 含义 | 驱动行为 | 阶段 |
|---|---|---|---|
| `EUACX_DRV_VOL_NO_READBACK` | 音量 / 静音 `GET_CUR` 不可信 | 不再读回，`euacx_get_volume` / `get_mute` 返回最后一次成功写入的值 | L1 |
| `EUACX_DRV_VOL_MIN_IS_MUTE` | 最小音量等于静音 | `euacx_info_t.volume_min` 上调一个步进；设置到最小值时改为静音 | L1 |
| `EUACX_DRV_VOL_RANGE` | 设备报告的范围错误 | 用 `params` 中的范围替换 `GET RANGE` 结果 | L1 |
| `EUACX_DRV_NO_HW_VOLUME` | 硬件音量无效或有害 | `has_volume = false`（软件音量不在本驱动范围内，由应用处理） | L1 |
| `EUACX_DRV_CTL_DELAY` | 控制请求太密会出错 | 每次类请求后等待 `ctl_delay_us` | L1 |
| `EUACX_DRV_RATE_NO_READBACK` | 采样率 `GET_CUR` 出错或不准 | 只发 `SET_CUR`，不比较读回值 | L2 |
| `EUACX_DRV_ALT_BEFORE_RATE` | 必须先选 alt 再设采样率 | 打开流的顺序改为 alt 0 → alt N → 设采样率 | L2 |
| `EUACX_DRV_IFACE_DELAY` | 切换 alt 后需要时间稳定 | `SET_INTERFACE` 后等待 `iface_delay_ms` | L2 |
| `EUACX_DRV_NATIVE_DSD` | 支持原生 DSD | 见第 7 节 | L4 |
| `EUACX_DRV_DSD_LE` | 原生 DSD 每个 32 位字节序相反 | 打包时每 4 字节反序 | L4 |
| `EUACX_DRV_DSD_BITREV` | DSD 字节内位序相反 | 打包时每字节位反转 | L4 |

- `euacx_driver_validate()` 增加检查：`VOL_RANGE` 必须有 `params` 且 `vol_min < vol_max`、`vol_res > 0`、范围是步进的整数倍；`CTL_DELAY` / `IFACE_DELAY` 的延时不为 0 且有上限（控制延时 ≤ 20 ms，接口延时 ≤ 200 ms）；`bcd_min ≤ bcd_max`。
- 驱动匹配增加 `bcdDevice` 范围条件：同一 VID:PID 下，带范围且命中的条目优先于不带范围的条目。
- 标志只改变行为，不改变公开接口；`euacx_info_t` 新增 `uint32_t driver_flags`（只读，便于应用和日志排查）。

## 5. 通用检查
不需要设备表，所有设备都执行。

### 5.1 音量与静音（L1）
- **范围修正**：`res == 0` 时按 1（1/256 dB）处理；`min == max` 时认为没有硬件音量；`GET RANGE` 返回多个子区间时只用第一个，并打印一次警告。
- **通道选择**：保持现有规则（在 master 和通道 1、2 中取可读写的，写入时所有可读写通道设为同一值），不改为"单通道优先、master 兜底"，以免改变 CX31993 已实测的行为。读回检测和缓存值都按现有规则选出的通道进行。
- **读回检测**：枚举时（尚未开流，不出声）对音量做一次检测：读当前值 → 写入相邻一个步进 → 读回 → 写回原值。
  - 读回等于写入值：正常。
  - 读回始终不变：驱动条目带 `VOL_NO_READBACK` 时改用缓存值；否则禁用硬件音量（`has_volume = false`）并打印 `VOLUME READBACK FAILED <vid>:<pid>`，提示可加入驱动表。
  - 检测本身的控制请求失败：禁用硬件音量，不影响枚举。
- **错误不影响播放**：音量 / 静音控制失败只影响对应接口的返回值，不导致枚举失败或流停止。
- **静音**：只在可读写时使用；静音读回不一致与音量按同样规则处理。

### 5.2 采样率与 alt（L2）
- **同格式 alt 按最小 MPS 选择**：通道数、位深、subslot、格式都相同的多个 alt 中，选能承载该采样率的最小 MPS，节省 FS 带宽。现有"位深最小、其次 subslot 最小"的规则在前。
- **`AS_VAL_ALT_SETTINGS`**：AS 接口描述符 `bmControls` 声明了"有效 alt"控制时，设好采样率后读取该位图，当前 alt 不在位图中就换下一个候选 alt；全部无效时返回 `ESP_ERR_NOT_SUPPORTED`。没有声明该控制的设备不发这个请求。
- **采样率读回**：保持现有严格比较；只有驱动条目带 `RATE_NO_READBACK` 时才跳过。
- 现有"alt 0 → 设采样率 → alt N"的顺序作为默认，不改变。

## 6. 设备条目的来源与证据等级
### 6.1 记录
新建 `docs/device-notes.md`，每个设备一节，用自己的话写：

```
## 2d99:a024 Edifier MF200
- 现象：音量 SET_CUR 生效，GET_CUR 始终返回同一个值
- 来源：<邮件列表或补丁说明的链接>
- 证据等级：reported
- 本项目条目：EUACX_DRV_VOL_NO_READBACK
- 实测：未实测
```

代码中的条目只写设备名，不写来源注释；来源只记录在本文档中。

### 6.2 证据等级
| 等级 | 条件 | 进入驱动表的方式 |
|---|---|---|
| `verified` | 本项目在 S3 或 P4 上实测确认 | 正常加入，可带已实测能力 |
| `reported` | 有公开的问题报告或补丁说明，描述了 VID:PID 和现象 | 加入，受 Kconfig `EUACX_DRV_REPORTED`（默认 y）控制；日志中标明 `reported` |
| `guess` | 只是同芯片、同厂商推测 | 不加入，只记录在 `device-notes.md` |

- 厂商通配条目（`pid = 0`）只用于 DSD 类标志，控制和流相关标志必须精确到 VID:PID。
- 每个条目单独一个提交，提交说明写明现象和证据等级。

### 6.3 候选清单（L3）
L3 先整理候选清单，只记录 VID:PID、设备名、现象、来源链接，优先级：
1. 手头已有、能实测的 DAC。
2. 基于常见方案的便宜 UAC2 DAC 和解码耳放（C-Media、Realtek、杰理、Savitech、Conexant / Synaptics、XMOS、ESS 方案），只支持 UAC1 的型号不收录。
3. 有公开报告的音量读回问题设备。耳机类设备（如 Logitech 游戏耳机）不是本项目目标，暂不加入。

## 7. 原生 DSD（L4，并入 2.0）
原生 DSD 没有统一标准，需要按设备配置以下几项：

| 项目 | 做法 |
|---|---|
| 哪个 alt 是 DSD | 默认：`bmFormats` 带 RAW_DATA、subslot 4、32 位，且驱动条目带 `NATIVE_DSD`。老设备把 DSD alt 声明为 32 位 PCM 时，用 `params.dsd_alt` 指定 alt 号 |
| 样本格式 | 默认每个 32 位 slot 装同一声道 32 个 DSD 位，按时间顺序先发较早的字节；`DSD_LE`、`DSD_BITREV` 处理例外 |
| 采样率 | 时钟设为 DSD 速率 × 44100 / 32（DSD64 = 88.2 kHz），与主计划一致 |
| 固件差异 | 同一 VID:PID 不同固件行为不同时，用 `bcd_min` / `bcd_max` 区分 |
| 模式切换 | 需要厂商请求切换 PCM / DSD 模式的设备，用新钩子 `select_mode(dev, ctx, is_dsd)`，在 alt 0 之后、设采样率之前调用，前后的等待时间由驱动自己决定 |
| 静音码 | 欠载和启动填充用 `0x69`，与主计划第 3.2 节一致 |

- 厂商通配条目（如 XMOS `20b1:*`）只启用"按 RAW_DATA 识别"，字节序等例外必须精确到 VID:PID。
- 主计划第 4.3 节"2.0 按实际适配的 DAC 新增"的规则不变：第一个原生 DSD 条目必须是 `verified`。

## 8. 不支持 UAC1
驱动只支持 UAC2。只支持 UAC1 的设备（如 CM108、PCM2704 方案）在枚举时按现有逻辑忽略，打印一行日志说明原因；设备表不收录 UAC1 设备，`docs/device-notes.md` 也不为其建条目。参考资料中 UAC1 专用的现象（如 `GET_MIN` / `GET_MAX` / `GET_RES`、对端点设置采样率）不纳入本计划。

## 9. 测试
| 模块 | 用例 |
|---|---|
| 许可证检查 | 构造含 `GPL-2.0` SPDX、`QUIRK_FLAG_` 标识符、`#include <linux/...>` 的临时文件，检查脚本均报错；正常源码通过 |
| 驱动条目 | `validate` 拒绝无效参数（范围、延时上限、`bcd_min > bcd_max`）；`bcdDevice` 范围优先于无范围条目；`EUACX_DRV_REPORTED = n` 时 `reported` 条目不匹配 |
| 音量范围 | `res = 0` 修正为 1；`min == max` 禁用；多子区间只取第一个；`VOL_RANGE` 覆盖设备范围；`VOL_MIN_IS_MUTE` 下最小值上调且设到最小时改为静音 |
| 读回检测 | 用模拟控制传输：正常设备通过并写回原值；读回不变且无标志时禁用硬件音量；带 `VOL_NO_READBACK` 时用缓存值；检测请求失败不影响枚举 |
| 通道选择 | 只有 master、只有单通道、两者都有三种描述符夹具，选出的通道与现有规则一致（回归） |
| UAC1 | UAC1 描述符夹具枚举时被忽略并打印原因，不进入驱动匹配 |
| alt 选择 | 同格式不同 MPS 时选最小可用 MPS；`AS_VAL_ALT_SETTINGS` 排除当前 alt 时换下一个；全部无效返回 `ESP_ERR_NOT_SUPPORTED` |
| 流顺序 | 记录控制请求顺序：默认顺序、`ALT_BEFORE_RATE` 顺序、`RATE_NO_READBACK` 不发 `GET_CUR`、延时标志生效 |
| 回归 | CX31993 的已有单元测试和实机矩阵全部仍然通过 |
| DSD（L4） | RAW_DATA 识别、`dsd_alt` 指定、默认 / `DSD_LE` / `DSD_BITREV` 三种打包、`select_mode` 调用位置 |

- 控制传输的模拟：在 `euacx_control` 下增加可替换的传输函数（只在主机测试中替换），按预设表返回数据并记录请求顺序。
- 每个 `verified` 条目在 S3 或 P4 上至少跑一遍矩阵和音量读回。

## 10. 里程碑
- **L0 规则**：第 2 节写入组件 README 的"贡献须知"；`check_license.ps1` 接入主机测试；建 `docs/device-notes.md`。验收：检查脚本在现有仓库上通过，在构造的违规文件上失败。
- **L1 控制**：驱动条目扩展、音量 / 静音通用检查和控制类标志。验收：第 9 节对应用例通过；CX31993 音量行为与 1.0 一致。
- **L2 流**：流相关标志、最小 MPS 选择、`AS_VAL_ALT_SETTINGS`。验收：第 9 节对应用例通过；CX31993 矩阵与 1.0 结果一致。
- **L3 条目**：候选清单整理完成；至少 1 个新的 `verified` 条目（需要一台新的杂牌 DAC）。
- **L4 DSD**：随主计划 2.0 实施。

## 11. 文档
- 组件 README：新增"设备特例"小节，说明标志位含义、如何新增条目、证据等级、`EUACX_DRV_REPORTED` 开关，以及"不接受 GPL 代码"的贡献须知。
- `docs/device-notes.md`：设备记录（第 6.1 节）。
- 根目录 README 的"参考"小节：说明设备行为信息参考了 Linux 社区的公开报告和 USB-IF 规范，本仓库不包含 Linux 代码。
- 主计划 `plan.zh.md`：在第 13 节"已确认的决定"中加一条指向本计划。

## 12. 实施记录（2026-10-06）

| 阶段 | 软件进度 | 尚待验收 |
|---|---|---|
| L0 | MIT 贡献规则、许可证扫描及八个违规样例、设备事实文档，已接入主机测试 | 无 |
| L1 驱动 | 八个控制/流标志、参数检查、bcdDevice 匹配优先级、reported 开关、info.driver_flags | 新条目仍需证据和实机 |
| L1 控制 | 范围修正及覆盖、固定范围禁用、首个通道可逆读回探测、成功写入缓存、最低音量映射静音、类请求延时；音量/静音错误隔离 | P4 已通过；S3 音量和播放矩阵待重跑 |
| L2 | 保留默认顺序；alt-first、采样率跳过读回、接口延时、最小 MPS、带长度的有效 alt 位图及候选回退；按最终 alt 分配缓冲及传输 | P4 CX31993 已通过；S3 及带有效 alt 控制的 DAC 待测 |
| L3 | [候选及来源记录](docs/device-notes.md) 已建立，尚无符合全部条件的新条目录入 | 至少一台新 UAC2 DAC 的 verified 条目 |
| L4 | 随 2.0；本轮未接入原生 DSD、DSD flags/params 和模式切换钩子 | 第一台原生 DSD DAC 实测 |

本轮仅独立编写自有实现，未导入 Linux 源码、内部名称或设备表。传输拆到 euacx_transport.c，生产控制代码由主机测试链接模拟传输、延时和接口 claim。无效候选用索引位图排除，不复制整份能力到 manager 栈，不修改已公布能力。

读回不可信的已声明条目不读取 CUR，枚举时成功写入最小可用音量和解除静音以建立缓存；如果多通道写入中途失败，缓存失效并返回错误，不能把部分写入当成成功。普通设备的探测只改动规则选出的首个通道，即使检测请求失败也尝试恢复原值。启动期间恢复静音失败只记录控制错误；未成功取得原静音状态时不使用默认值恢复，最低音量转静音的成功请求也会更新启动后的恢复目标。

验证结果：默认配置和关闭 CX31993/reported 配置各 **55 项主机测试，0 失败**；许可证检查在现有源码上通过，八个违规临时样例均被拒绝。ESP-IDF 5.5.1 / USB Host 1.4.1 下 S3/P4 示例编译通过，改动模块无编译警告；diff 空白检查通过。

软件测试及两板编译通过后仍保留 1.0.0-rc.1，不代表 1.1 或正式 1.0 发布完成；软件开发阶段未烧录，随后按用户要求完成下述 P4 实机回归；没有新增 verified 设备。待硬件验收的 L1/L2 和缺新设备的 L3 保持 in_progress。

### P4 烧录回归（2026-10-06）

按用户要求通过 COM10 烧录 ESP32-P4 v1.3，使用独立 Matrix 配置，关闭已验证能力过滤，遍历描述符能力。Flash 哈希校验通过，重启后 CX31993 `06cb:1594` 以 HS 枚举，driver_flags=0。

- 板上 Unity：39 项，0 失败、0 忽略。
- 音量/静音：has_volume=1、has_mute=1，范围 [−18944, 0]/128（1/256 dB）；枚举读回检测未报错，跨任务音量 SET/GET、推送 owner/abort、阻塞拉取 close 均 PASS，音量请求 0 ms。
- PCM16/24/32：各 8/16/32/44.1/48/96/192/384 kHz，推送和拉取共 48 次全部 PASS；全部 ISO 错误为 0，所有拉取欠载为 0。推送末尾主动等待播放完毕的静音期会计入欠载，不据此宣称推送零欠载。
- `scripts/check_validation.py` 严格日志检查全部通过；无控制超时、读回失败或 panic。用户在播放期间确认“声音正常”。
- 原始日志保存在本机忽略的 `tmp/linux-p4-runtime.log`，烧录记录为 `tmp/linux-p4-flash.log`。

这次验证现有 CX31993 的默认路径回归，不替代特例设备、AS 有效 alt 位图设备、S3、物理热插拔或 30 分钟持续播放的验收；版本仍保留 1.0.0-rc.1。

### S3 烧录回归开始（2026-10-06，串口中断，尚未完成）

通过 CH343 COM9 烧录 ESP32-S3 v0.2 的 Matrix 配置，Flash 哈希校验通过。重启后板上 Unity 39 项通过、0 失败，CX31993 `06cb:1594` 以 FS 枚举，flags=0，音量/静音可用，范围 [−18944, 0]/128。推送 owner/abort/volume 和阻塞拉取 close/volume 均 PASS，音量请求 0 ms。Host 因 MPS768 超过 FIFO 能力排除 PCM32。

已记录 PCM16 六档推送/拉取共 12 次、PCM24 的 8/16/32/44.1/48 kHz 推送/拉取共 10 次，合计 22 次 PASS；已完成流的 ISO 错误为 0，所有已完成拉取欠载为 0，无音量/静音读回失败。PCM24/96 kHz 开始时串口读取失败，COM9 随后从系统端口列表消失，因此缺少最后两次结果和最终矩阵汇总，不能标记完整 S3 验收通过。

原始未完整日志为本机忽略的 `tmp/linux-s3-runtime.log`，采集错误为 `tmp/linux-s3-capture.log`，烧录记录为 `tmp/linux-s3-flash.log`。待连接恢复后重跑完整矩阵；本次声音尚未得到用户确认。
