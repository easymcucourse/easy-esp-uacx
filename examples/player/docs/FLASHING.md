# 固件烧录与 OTA

## 首次完整烧录

项目使用 16 MB Flash 和双 OTA 分区。首次烧录或修改 Bootloader、分区表、Flash
配置后，需要完整写入：

```powershell
idf.py -p <串口> -b 230400 flash
```

也可以直接调用 `esptool.py`：

```powershell
esptool.py --chip esp32s3 --port <串口> --baud 230400 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 build/bootloader/bootloader.bin `
  0x8000 build/partition_table/partition-table.bin `
  0xF000 build/ota_data_initial.bin `
  0x20000 build/esp32_s3_music_player.bin
```

如果开发板使用自定义 RTS/DTR 自动下载器，请在该下载器自己的文档和配置目录中维护
复位时序，不要把本机绝对路径写入本仓库。

## TCP OTA

在 `idf.py menuconfig` 中配置 Wi-Fi 并至少完整烧录一次后，可只更新应用分区：

```powershell
python tcp_ota.py <设备IP> build/esp32_s3_music_player.bin
```

成功时会显示 `OTA READY` 和 `OTA OK REBOOTING`。TCP OTA 不能更新 Bootloader 或
分区表。

> 安全提示：当前 TCP OTA 没有身份验证和 TLS。只应在可信、隔离的局域网中启用，
> 不要把端口转发到互联网。

## 常见问题

- 找不到串口：在 Windows 中运行
  `Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name`。
- 高波特率写入中断：将烧录波特率降低到 `230400` 或 `115200`。
- 应用无法启动：确认使用当前 `partitions.csv`，应用分区起始地址为 `0x20000`。
