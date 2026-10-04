#include "sd_storage.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "board_pins.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sd_protocol_defs.h"

#define SD_SECTOR_SIZE       512U
#define SD_TEST_SECTORS      8U
#define SD_TEST_ITERATIONS   16U
#define SD_TEST_BYTES        (SD_SECTOR_SIZE * SD_TEST_SECTORS)

static const char *TAG = "sd_diag";
static const uint32_t s_test_speeds_khz[] = {
    400, 1000, 4000, 10000, 20000, 40000,
};

static spi_bus_config_t make_bus_config(void)
{
    const spi_bus_config_t config = {
        .mosi_io_num = MUSIC_SD_PIN_MOSI,
        .miso_io_num = MUSIC_SD_PIN_MISO,
        .sclk_io_num = MUSIC_SD_PIN_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SD_TEST_BYTES,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };
    return config;
}

static sdspi_device_config_t make_device_config(void)
{
    sdspi_device_config_t config = SDSPI_DEVICE_CONFIG_DEFAULT();
    config.host_id = MUSIC_SD_SPI_HOST;
    config.gpio_cs = MUSIC_SD_PIN_CS;
    config.gpio_cd = SDSPI_SLOT_NO_CD;
    config.gpio_wp = SDSPI_SLOT_NO_WP;
    return config;
}

static uint32_t checksum_fnv1a(const uint8_t *data, size_t length)
{
    uint32_t value = UINT32_C(2166136261);
    for (size_t index = 0; index < length; ++index) {
        value ^= data[index];
        value *= UINT32_C(16777619);
    }
    return value;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void print_bpb_probe(const uint8_t *sector)
{
    char oem[9];
    memcpy(oem, sector + 3, 8);
    oem[8] = '\0';
    for (size_t index = 0; index < 8; ++index) {
        if ((uint8_t)oem[index] < 0x20 || (uint8_t)oem[index] > 0x7e) {
            oem[index] = '.';
        }
    }

    ESP_LOGI(TAG,
             "boot sector: jump=%02x %02x %02x OEM='%s' bps=%u spc=%u "
             "reserved=%u FATs=%u root_entries=%u media=0x%02x "
             "fat16_sectors=%u total16=%u total32=%" PRIu32,
             sector[0], sector[1], sector[2], oem,
             read_le16(sector + 11), sector[13], read_le16(sector + 14),
             sector[16], read_le16(sector + 17), sector[21],
             read_le16(sector + 22), read_le16(sector + 19),
             read_le32(sector + 32));
}

static const char *filesystem_name(const uint8_t *sector)
{
    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        return "exFAT";
    }
    if (memcmp(sector + 82, "FAT32   ", 8) == 0) {
        return "FAT32";
    }
    if (memcmp(sector + 54, "FAT16   ", 8) == 0 ||
        memcmp(sector + 54, "FAT12   ", 8) == 0) {
        return "FAT12/16";
    }
    return "unknown/non-FAT";
}

static bool is_valid_fat_boot_sector(const uint8_t *sector)
{
    const uint16_t bytes_per_sector = read_le16(sector + 11);
    const uint8_t sectors_per_cluster = sector[13];
    const uint16_t reserved_sectors = read_le16(sector + 14);
    const uint8_t fat_count = sector[16];
    const uint16_t root_entries = read_le16(sector + 17);
    const uint16_t fat16_sectors = read_le16(sector + 22);
    const uint32_t fat32_sectors = read_le32(sector + 36);
    const uint32_t total_sectors = read_le16(sector + 19) != 0
                                       ? read_le16(sector + 19)
                                       : read_le32(sector + 32);
    const bool valid_jump = sector[0] == 0xeb || sector[0] == 0xe9;
    const bool valid_cluster =
        sectors_per_cluster != 0 &&
        (sectors_per_cluster & (sectors_per_cluster - 1U)) == 0;
    const bool valid_fat_layout =
        (root_entries == 0 && fat32_sectors != 0) ||
        (root_entries != 0 && fat16_sectors != 0);

    return valid_jump && bytes_per_sector == SD_SECTOR_SIZE &&
           valid_cluster && reserved_sectors != 0 &&
           (fat_count == 1 || fat_count == 2) && total_sectors != 0 &&
           valid_fat_layout && sector[510] == 0x55 && sector[511] == 0xaa;
}

static esp_err_t probe_filesystem_addressing(sdmmc_card_t *card,
                                             uint8_t *sector,
                                             bool verbose,
                                             uint32_t *test_lba)
{
    esp_err_t error = sdmmc_read_sectors(card, sector, 0, 1);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "cannot read LBA0 for filesystem probe: %s",
                 esp_err_to_name(error));
        return error;
    }

    const bool has_signature = sector[510] == 0x55 && sector[511] == 0xaa;
    const char *direct_name = filesystem_name(sector);
    if (is_valid_fat_boot_sector(sector)) {
        *test_lba = 0;
        if (verbose) {
            print_bpb_probe(sector);
            ESP_LOGI(TAG,
                     "filesystem probe: superfloppy %s, boot signature=55AA",
                     direct_name);
        }
        return ESP_OK;
    }

    const uint8_t partition_type = sector[450];
    const uint32_t partition_lba = read_le32(sector + 454);
    if (!has_signature || partition_type == 0 || partition_lba == 0) {
        ESP_LOGW(TAG,
                 "filesystem probe: LBA0 is %s, no usable MBR partition "
                 "(signature=%s type=0x%02x lba=%" PRIu32 ")",
                 direct_name, has_signature ? "55AA" : "missing",
                 partition_type, partition_lba);
        return ESP_ERR_NOT_FOUND;
    }

    const uint32_t original_ocr = card->ocr;
    error = sdmmc_read_sectors(card, sector, partition_lba, 1);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "cannot read partition boot sector LBA=%" PRIu32 ": %s",
                 partition_lba, esp_err_to_name(error));
        return error;
    }

    if (!is_valid_fat_boot_sector(sector) &&
        (original_ocr & SD_OCR_SDHC_CAP) != 0) {
        /*
         * Some non-compliant 2 GB cards set the SDHC/CCS bit but continue to
         * interpret CMD17/CMD24 arguments as byte addresses.  ESP-IDF then
         * sends LBA instead of LBA*512.  Retry only after a valid MBR points
         * at a boot sector which fails strict BPB validation; retain the
         * workaround only if the byte-addressed read yields a valid FAT BPB.
         */
        card->ocr &= ~SD_OCR_SDHC_CAP;
        error = sdmmc_read_sectors(card, sector, partition_lba, 1);
        if (error != ESP_OK || !is_valid_fat_boot_sector(sector)) {
            card->ocr = original_ocr;
            if (error != ESP_OK) {
                return error;
            }
        } else if (verbose) {
            ESP_LOGW(TAG,
                     "SD ADDRESS COMPAT PASS: card reports SDHC block "
                     "addressing but requires byte addressing (LBA*512)");
        }
    }

    *test_lba = partition_lba;
    if (verbose) {
        print_bpb_probe(sector);
        ESP_LOGI(TAG,
                 "filesystem probe: MBR type=0x%02x start=%" PRIu32
                 ", volume=%s, boot signature=%s, addressing=%s",
                 partition_type, partition_lba, filesystem_name(sector),
                 (sector[510] == 0x55 && sector[511] == 0xaa)
                     ? "55AA"
                     : "missing",
                 (card->ocr & SD_OCR_SDHC_CAP) != 0
                     ? "SDHC block"
                     : "compat byte");
    }
    return is_valid_fat_boot_sector(sector) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t test_speed(uint32_t requested_khz, bool print_card)
{
    esp_err_t error;
    bool bus_initialized = false;
    bool host_initialized = false;
    bool device_initialized = false;
    sdspi_dev_handle_t device = -1;
    uint8_t *buffer = NULL;
    sdmmc_card_t card = {0};
    int real_freq_khz = 0;
    uint32_t test_lba = 0;

    spi_bus_config_t bus_config = make_bus_config();
    error = spi_bus_initialize(MUSIC_SD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%" PRIu32 " kHz: SPI bus init failed: %s",
                 requested_khz, esp_err_to_name(error));
        goto cleanup;
    }
    bus_initialized = true;

    error = sdspi_host_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%" PRIu32 " kHz: SDSPI host init failed: %s",
                 requested_khz, esp_err_to_name(error));
        goto cleanup;
    }
    host_initialized = true;

    sdspi_device_config_t device_config = make_device_config();
    error = sdspi_host_init_device(&device_config, &device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%" PRIu32 " kHz: card device init failed: %s",
                 requested_khz, esp_err_to_name(error));
        goto cleanup;
    }
    device_initialized = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = device;
    host.max_freq_khz = requested_khz;
    error = sdmmc_card_init(&host, &card);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%" PRIu32 " kHz: card init failed: %s",
                 requested_khz, esp_err_to_name(error));
        goto cleanup;
    }

    (void)sdspi_host_get_real_freq(device, &real_freq_khz);
    if (print_card) {
        sdmmc_card_print_info(stdout, &card);
    }

    buffer = heap_caps_malloc(SD_TEST_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    const bool reported_sdhc = (card.ocr & SD_OCR_SDHC_CAP) != 0;
    const esp_err_t probe_error = probe_filesystem_addressing(
        &card, buffer, print_card, &test_lba);
    if (probe_error != ESP_OK && print_card) {
        ESP_LOGW(TAG,
                 "no valid FAT boot sector found; raw speed test uses LBA0");
        test_lba = 0;
    }

    uint32_t expected_checksum = 0;
    bool checksum_ready = false;
    const int64_t start_us = esp_timer_get_time();
    for (uint32_t iteration = 0; iteration < SD_TEST_ITERATIONS; ++iteration) {
        error = sdmmc_read_sectors(&card, buffer, test_lba, SD_TEST_SECTORS);
        if (error != ESP_OK) {
            ESP_LOGE(TAG,
                     "%" PRIu32 " kHz: read failed on pass %" PRIu32 ": %s",
                     requested_khz, iteration + 1, esp_err_to_name(error));
            goto cleanup;
        }
        const uint32_t checksum = checksum_fnv1a(buffer, SD_TEST_BYTES);
        if (!checksum_ready) {
            expected_checksum = checksum;
            checksum_ready = true;
        } else if (checksum != expected_checksum) {
            ESP_LOGE(TAG,
                     "%" PRIu32 " kHz: inconsistent data on pass %" PRIu32
                     " (expected=%08" PRIx32 " got=%08" PRIx32 ")",
                     requested_khz, iteration + 1, expected_checksum, checksum);
            error = ESP_FAIL;
            goto cleanup;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const uint64_t total_bytes =
        (uint64_t)SD_TEST_BYTES * SD_TEST_ITERATIONS;
    const uint64_t kib_per_second = elapsed_us > 0
                                        ? total_bytes * 1000000ULL /
                                              (uint64_t)elapsed_us / 1024ULL
                                        : 0;
    ESP_LOGI(TAG,
             "SDSPI TEST PASS: mode=0 requested=%" PRIu32
             " kHz actual=%d kHz read=%" PRIu64
             " KiB/s checksum=%08" PRIx32 " addressing=%s",
             requested_khz, real_freq_khz, kib_per_second, expected_checksum,
             reported_sdhc && (card.ocr & SD_OCR_SDHC_CAP) == 0
                 ? "compat-byte"
                 : "standard");
    error = ESP_OK;

cleanup:
    free(buffer);
    if (device_initialized) {
        (void)sdspi_host_remove_device(device);
    }
    if (host_initialized) {
        (void)sdspi_host_deinit();
    }
    if (bus_initialized) {
        (void)spi_bus_free(MUSIC_SD_SPI_HOST);
    }
    return error;
}

uint32_t sd_storage_run_speed_sweep(void)
{
    ESP_LOGI(TAG,
             "SD SPI protocol requires CPOL=0/CPHA=0 (Mode 0); "
             "Modes 1/2/3 are invalid and are skipped");
    ESP_LOGI(TAG,
             "read-only sweep: 8 sectors x 16 passes at "
             "0.4/1/4/10/20/40 MHz");

    uint32_t selected_khz = 0;
    for (size_t index = 0;
         index < sizeof(s_test_speeds_khz) / sizeof(s_test_speeds_khz[0]);
         ++index) {
        const uint32_t speed_khz = s_test_speeds_khz[index];
        if (test_speed(speed_khz, index == 0) == ESP_OK &&
            speed_khz <= MUSIC_SD_RUN_FREQ_KHZ) {
            selected_khz = speed_khz;
        }
    }

    if (selected_khz == 0) {
        ESP_LOGE(TAG, "no stable SDSPI speed at or below %d kHz",
                 MUSIC_SD_RUN_FREQ_KHZ);
    } else {
        ESP_LOGI(TAG, "selected runtime SDSPI speed: %" PRIu32 " kHz",
                 selected_khz);
    }
    return selected_khz;
}

esp_err_t sd_storage_mount(const char *mount_point, uint32_t speed_khz,
                           sdmmc_card_t **out_card)
{
    esp_err_t error = ESP_FAIL;
    bool bus_initialized = false;
    bool host_initialized = false;
    bool device_initialized = false;
    bool disk_registered = false;
    bool vfs_registered = false;
    sdspi_dev_handle_t device = -1;
    sdmmc_card_t *card = NULL;
    uint8_t *probe_sector = NULL;
    BYTE physical_drive = FF_DRV_NOT_USED;
    FATFS *filesystem = NULL;
    char drive[3] = {0};

    spi_bus_config_t bus_config = make_bus_config();
    error = spi_bus_initialize(MUSIC_SD_SPI_HOST, &bus_config,
                               SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mount SPI bus init failed: %s", esp_err_to_name(error));
        return error;
    }
    bus_initialized = true;

    error = sdspi_host_init();
    if (error != ESP_OK) {
        goto cleanup;
    }
    host_initialized = true;

    sdspi_device_config_t device_config = make_device_config();
    error = sdspi_host_init_device(&device_config, &device);
    if (error != ESP_OK) {
        goto cleanup;
    }
    device_initialized = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = device;
    host.max_freq_khz = speed_khz;
    card = calloc(1, sizeof(*card));
    if (card == NULL) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    error = sdmmc_card_init(&host, card);
    if (error != ESP_OK) {
        goto cleanup;
    }

    probe_sector = heap_caps_malloc(SD_SECTOR_SIZE,
                                    MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (probe_sector == NULL) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    uint32_t filesystem_lba = 0;
    error = probe_filesystem_addressing(card, probe_sector, true,
                                        &filesystem_lba);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "FAT addressing probe failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }
    free(probe_sector);
    probe_sector = NULL;

    error = ff_diskio_get_drive(&physical_drive);
    if (error != ESP_OK || physical_drive == FF_DRV_NOT_USED) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ff_diskio_register_sdmmc(physical_drive, card);
    ff_sdmmc_set_disk_status_check(physical_drive, false);
    disk_registered = true;
    drive[0] = (char)('0' + physical_drive);
    drive[1] = ':';

    const esp_vfs_fat_conf_t vfs_config = {
        .base_path = mount_point,
        .fat_drive = drive,
        .max_files = 8,
    };
    error = esp_vfs_fat_register_cfg(&vfs_config, &filesystem);
    if (error != ESP_OK) {
        goto cleanup;
    }
    vfs_registered = true;

    const FRESULT mount_result = f_mount(filesystem, drive, 1);
    if (mount_result != FR_OK) {
        ESP_LOGE(TAG, "FAT mount failed: FatFs result=%d", mount_result);
        error = ESP_FAIL;
        goto cleanup;
    }

    if (out_card != NULL) {
        *out_card = card;
    }

    ESP_LOGI(TAG,
             "SDSPI FAT mount PASS at %" PRIu32 " kHz, addressing=%s",
             speed_khz,
             (card->ocr & SD_OCR_SDHC_CAP) != 0
                 ? "SDHC block"
                 : "compat byte (LBA*512)");
    sdmmc_card_print_info(stdout, card);
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(mount_point, &total, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "SD filesystem: total=%" PRIu64 " MiB free=%" PRIu64 " MiB",
                 total / (1024 * 1024), free_bytes / (1024 * 1024));
    }
    return ESP_OK;

cleanup:
    free(probe_sector);
    if (vfs_registered) {
        (void)f_mount(NULL, drive, 0);
        (void)esp_vfs_fat_unregister_path(mount_point);
    }
    if (disk_registered) {
        ff_diskio_unregister(physical_drive);
    }
    free(card);
    if (device_initialized) {
        (void)sdspi_host_remove_device(device);
    }
    if (host_initialized) {
        (void)sdspi_host_deinit();
    }
    if (bus_initialized) {
        (void)spi_bus_free(MUSIC_SD_SPI_HOST);
    }
    ESP_LOGE(TAG, "SDSPI FAT mount failed at %" PRIu32 " kHz: %s",
             speed_khz, esp_err_to_name(error));
    return error;
}
