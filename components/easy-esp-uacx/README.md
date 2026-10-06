# easy UAC2 组件 API

当前为 **1.0.0-rc.1**。PCM 运行时已实现；正式版本的实机验收进度见 [根 README](../../README.md) 和 [示例说明](../../examples/README.md)。唯一公开头文件是 `easy_uacx.h`，应用不需要包含 USB、FreeRTOS 或驱动表头文件。

## 初始化、端口与能力

```c
euacx_config_t config = EUACX_CONFIG_DEFAULT();
config.cb.on_connected = connected;
config.cb.on_disconnected = disconnected;
config.cb.on_stream_stopped = stopped;
ESP_ERROR_CHECK(euacx_init(&config));
```

`euacx_init` 创建 manager、USB 事件 pump、状态回调任务，并默认安装 USB Host 库及库事件任务。重复初始化返回 `ESP_ERR_INVALID_STATE`。任务优先级必须低于 pump，核编号是 `-1` 或有效 CPU 编号；供数任务优先级也必须低于 pump。

当前 `euacx_port_count()` 返回 1，`euacx_get_port(0)` 返回默认根端口句柄，越界返回 NULL。句柄在 init/deinit 之间保持有效，拔插不会更换句柄；deinit 后不得再保存使用旧句柄。S3 使用默认 FS Host，P4 使用默认 HS Host，第二个 P4 FS 根端口未实现。

`euacx_get_info` 复制能力快照，只在 CONNECTED/STREAMING 状态成功。每次枚举增加 `conn_id`，应用处理延迟消息时应检查该值。`pcm[0/1/2]` 是 **16/24/32 位输入**各自的升序速率列表，转换后的设备容器可能更宽。默认已验证模式使用驱动表与描述符/带宽的交集；未登记专用驱动的设备使用 generic 描述符能力。GET RANGE 不可用时公布标准候选速率，开流通过 SET/GET 确认设备是否接受。

## PCM 数据与播放

| 输入 | 立体声帧大小 | 布局 |
|---|---|---|
| 16 位 | 4 字节 | L16、R16，小端有符号 |
| 24 位 | 6 字节 | packed L24、R24，小端有符号 |
| 32 位 | 8 字节 | L32、R32，小端有符号 |

驱动选择能容纳输入位深的最窄设备格式，向更宽容器转换时高位对齐、低位补零，不降位深、不重采样。24 位输入映射到 4 字节容器时最低字节为零。已验证模式开流会保留表中登记的 subslot，避免能力查询通过后选择另一个未经验证的容器。

开流期间硬件静音保护时钟/alt 切换，先提交静音 ISO 管线，再恢复应用原有静音状态并消费 PCM；close 切回 alt0 时也保护静音。枚举时临时 claim/release 各 alt，检测实际控制器/FIFO 限制，不能只用 USB 总线理论带宽筛选。

推送模式：

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

仅打开流的任务可以 write/close 推送流。写入会复制到内部缓冲，返回后应用可复用原内存；`written` 包括已接收的未完整帧字节。超时单位 ms，0 不等待，`UINT32_MAX` 无限等待。输入可跨调用拼帧；close 是立即停止，未完整帧和未播放缓冲丢弃，推送没有 EOF 排空接口。

拉取模式在配置中设置 `on_data` 和 `data_user`。驱动在独立 `euacx_feed` 任务调用 `on_data(port, buf, len, user)`：返回 1..len 个输入字节、0 暂无数据、`EUACX_DATA_END` 结束。供数不必帧对齐；结束时残帧补零，缓冲和在途传输排空后发出 `STOP_EOF`。返回其他负值或超出 len 导致 `STOP_ERROR`。回调应尽快返回；返回 0 时驱动重试，运行中的缓冲不足会发静音并计欠载。

| 调用环境 | 约定 |
|---|---|
| 状态回调任务 `euacx_cb` | 按顺序运行 connected/stopped/disconnected，可调用公开 API；info 指针只在该回调内有效 |
| 推送流拥有任务 | write、close；可在不同调用间提供任意长度字节 |
| 其他应用任务 | 可查询、调音量和 abort；不可 write/close 别的任务的推送流 |
| 拉取供数任务 | 不可 write、close/open 同一端口或 deinit；可以查询和调音量 |
| 拉取流的其他任务 | 可 close；等待供数回调退出和 USB 传输取消后完成 |

`euacx_stream_abort` 不等待。推送模式下唤醒阻塞 write，使其返回 `ESP_ERR_INVALID_STATE`，流仍保持 STREAMING，随后由拥有任务 close；拉取模式下请求关闭。abort/close 在没有流时可重复调用。状态回调若长时间阻塞，通知暂存，manager 和 USB pump 继续运行；成功开流/枚举时已预留终止通知节点，队列满时按 FIFO 溢出保存。

`euacx_stream_close` 同步等待流释放。供数回调阻塞时，close 请求延迟完成，manager 仍可处理其他请求（例如音量）；应用必须让回调最终返回。`euacx_deinit` 停止流、释放设备和任务，只卸载自己安装的 Host；从状态回调调用时，最终内存释放推迟到回调退出。deinit 不会强制删除应用正在执行的供数回调。

## 状态与错误

状态路径：`DISCONNECTED → ENUMERATING → CONNECTED ⇄ STREAMING`，拔出后返回 DISCONNECTED。拔出、达到连续 ISO 错误阈值、close 或 EOF 会为已成功打开的流发送一次 stopped；有连接通知的设备随后发送对应 `conn_id` 的 disconnected。终止理由为 `EOF/CLOSED/UNPLUGGED/ERROR`；只有 ERROR 携带流错误。

| 错误 | 含义 |
|---|---|
| `ESP_ERR_INVALID_ARG` | 空参数、无效初始化配置或 PCM 格式参数 |
| `ESP_ERR_INVALID_STATE` | 未初始化、未连接、已开流、abort 后写入、任务权限不符等 |
| `ESP_ERR_NOT_SUPPORTED` | 不支持的位深/采样率、无音量/静音控制、DSD/DoP |
| `ESP_ERR_TIMEOUT` | 写入等待空间或 API 请求入队超时；控制传输超时也会触发设备恢复 |
| `ESP_ERR_NO_MEM` | 流缓冲、传输、通知或任务资源分配失败 |

API 请求入队成功后等待 manager 回复，不会让引用调用者栈内存的请求提前超时返回。内部 USB 事件使用地址/拔出标志作为队列满时的保底，优先于应用请求处理。控制传输有独立堆内存和引用计数，超时后等 USB 完成回调再释放。

## 硬件音量与静音

`has_volume/has_mute` 表示播放通路上相应控制可读写。音量是有符号 **1/256 dB**，例如 −10.5 dB = −2688。set 会裁剪到 min/max 并按 res 向下对齐，向支持的主/左右通道发送 SET，再 GET 读回确认；get 返回首个支持通道。多个 RANGE 子区间使用第一个并警告；res 为 0 时按 1/256 dB 处理，固定或无效范围禁用硬件音量。枚举期间对首个可读写通道执行相邻步进检测并恢复原值，读回不可信时禁用对应控制。音量/静音失败不阻止枚举或停止播放（设备掉线仍按正常断开处理）。

CX31993 专用驱动仅匹配 `06cb:1594`，初始音量 −10.5 dB；generic 的检测会恢复设备原音量。已声明读回不可信的设备在枚举时写入最小可用音量建立缓存，不读取 CUR。支持的静音控制在枚举初始化时解除。非 ASCII 产品字符串当前用 `?` 代替。

## 配置

| 配置 | 默认 | 说明 |
|---|---|---|
| `EUACX_DRV_CX31993` | y | 专用驱动 |
| `EUACX_DRV_REPORTED` | y | 允许公开报告支持的条目；当前尚无符合全部录入条件的新条目 |
| `EUACX_USE_VERIFIED_CAPS` | y | 使用已验证能力表（有表时） |
| `EUACX_BUFFER_MS` | 40 | 缓冲时长下限；按管线容量和 2 的幂扩容 |
| `EUACX_NUM_TRANSFERS` | 4 | 在途 ISO 传输数，2–8 |
| `EUACX_MAX_ERRORS` | 8 | 连续 ISO 错误停止阈值 |
| `EUACX_TASK_STACK` | 8192 | manager 栈字节数 |
| `EUACX_LIB_PRIORITY` | 20 | 自有 Host 库任务优先级 |
| `EUACX_PUMP_PRIORITY/STACK` | 19 / 8192 | USB 完成事件和初次提交任务 |
| `EUACX_INT_QUEUE_LEN` | 8 | USB 内部事件队列长度 |
| `EUACX_REQ_QUEUE_LEN/TIMEOUT_MS` | 8 / 2000 | 应用请求队列及入队等待 |
| `EUACX_CB_STACK/QUEUE_LEN` | 4096 / 16 | 状态回调任务栈和通知队列 |
| `EUACX_FEED_PRIORITY/STACK` | 10 / 8192 | 拉取供数任务 |
| `EUACX_FEED_RETRY_MS` | 2 | 返回 0 后的重试等待（加一个调度 tick） |
| `EUACX_RECOVERY_MS` | 5000 | 无 DAC 的根端口恢复周期，0 关闭 |
| `EUACX_DUMP_DESCRIPTORS` | n | 输出接受的配置描述符，便于建立真实测试夹具 |

公开依赖只有 `esp_common`；USB/FreeRTOS/heap/esp_rom 是私有依赖。USB Host 固定 1.4.1，IDF ≥5.4，当前验证版本为 5.5.1。外部 Host 模式下，应用必须先安装 USB Host 并持续泵送库事件。

## 新增驱动与测试

在 `drivers/drv_<name>.c` 定义 `euacx_driver_t`，在 `euacx_drivers.c` 登记，并按需增加 Kconfig/CMake 条件。精确 VID:PID 优先于厂商通配，最后为 generic；同一产品下命中的 bcdDevice 范围条目优先于无范围条目。控制和流特例必须精确到产品，不允许厂商通配。驱动可使用 attach/detach、fixup_caps 和 stream_start/stop 钩子；这些钩子运行于 manager，不能同步调用公开 API。

已验证表按 FS/HS 分开，记录输入位深、实际 subslot 和严格升序的采样率；只录入实际听音或录音检查、传输通过的组合。`euacx_driver_validate` 检查条目。CX31993 FS 表登记本轮 S3 PCM16（subslot2）和 PCM24（subslot3）的 8/16/32/44.1/48/96 kHz；HS 表登记 P4 PCM16/24/32（subslot2/3/4）各八档，另含 192/384 kHz，经 48 次播放和 UR22C input 1/2 录音对照验证。FS 的 PCM32 端点 MPS768 超过 S3 实测 FIFO600，因此被枚举阶段的 host claim 探测排除。

`test_pcm_model.c` 包含真实 CX31993 FS 描述符夹具，并测试播放时钟/FU、带宽、验证表与开流一致性、PCM 字节、反馈、RANGE、状态权限及计数器回绕。主机脚本还编译生产控制模块，替换传输/延时/接口 claim 以测试设备行为；板上还执行实际 USB 库重复生命周期和生产写入函数的拼帧/超时/abort 测试。示例用公开 API 验证跨任务行为、完整矩阵和长时间播放。

显式反馈目前有算法测试，仍需异步 DAC 实机验收；未支持隐式反馈、复杂时钟路由和可选 P4 双根端口。旧 DoP/native DSD 算法及测试作为 2.0 私有参考保留，不接入 1.0 播放 API。

## 设备特例与贡献须知

按 [Linux 参考计划](../../plan-linux-quirks.zh.md) 实现 L0–L2 软件部分；L3 的新设备实测和 L4 原生 DSD 仍待后续。设备事实及出处集中在 [device-notes](../../docs/device-notes.md)，代码只保留名称和参数。`info.driver_flags` 用于查看匹配条目的行为；标志定义和 params 位于私有 `euacx_driver.h`。

| 标志（`EUACX_DRV_` 前缀） | 行为 |
|---|---|
| `VOL_NO_READBACK` | 音量/静音跳过 CUR 读回，get 返回最后成功写入的缓存；部分通道写入失败后缓存无效 |
| `VOL_MIN_IS_MUTE` | 公布的最小音量上调一个步进，设置到设备原最小值或更低时改为静音；需要可写静音 |
| `VOL_RANGE` | params 提供 min/max/res，跳过设备 RANGE |
| `NO_HW_VOLUME` | 不公布硬件音量 |
| `CTL_DELAY` | 每次类请求后等待，1–20000 μs；毫秒级延时按调度 tick 向上取整 |
| `RATE_NO_READBACK` | 设采样率后跳过频率 CUR 比较，保留时钟有效性检查 |
| `ALT_BEFORE_RATE` | alt 0 → alt N → 设置采样率 |
| `IFACE_DELAY` | SET_INTERFACE 到非零 alt 后等待 1–200 ms |

默认顺序保持 alt 0 → 设置并读回采样率 → alt N。同位深/subslot 的 PCM alt 按最小可用 MPS 选择；设备声明有效 alt 控制时，设好频率后读取带长度字节的位图，排除无效候选，再尝试下一个。失败时回到 alt 0 并释放接口。缓冲及 ISO 传输按最终选中的 alt 分配。

新增条目先写事实记录，再独立实现。`verified` 必须有本项目 S3/P4 实测，能力表还需听音或录音及传输验证；`reported` 必须有包含 USB ID 和现象的公开报告，设置 `.reported = true`，受 `EUACX_DRV_REPORTED` 控制且连接日志标明证据等级；`guess` 只进文档。协议版本未确定、现象无法对应本项目标志时暂不录入。每个新设备条目应独立提交并在提交说明写明现象和证据。

本项目采用 MIT，不接受 Linux GPL 代码、头文件、宏、设备表或对其进行翻译/逐段改写；不得以 Linux 内部名称建立代码对应关系，也不得在编写实现时打开其源代码对照。Linux 社区报告只用于提取设备事实，协议实现依据 USB-IF 规范；不导入其他项目源码。2026-10-06 默认及关闭 CX31993/reported 两种配置各 55 项主机测试通过，S3/P4 示例编译通过；随后 P4 v1.3 上 39 项板上测试及 CX31993 的 48 次矩阵播放通过，拉取零错误/零欠载，用户确认声音正常；S3 与新特例设备仍待验收。主机测试先执行 `scripts/check_license.ps1` 和八个违规样例，扫描自有 components/examples/scripts 的源文件，排除第三方、文档和构建产物。
