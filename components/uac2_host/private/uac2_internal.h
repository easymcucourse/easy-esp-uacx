#pragma once
#include "uac2_types.h"
#include "uac2_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uac2_stream uac2_stream_t;

/* ---- Format driver vtable (PCM / DoP / Native DSD) ---- */
typedef struct {
    esp_err_t (*probe)(const uac2_device_caps_t *dev, const uac2_stream_cap_t *cap,
                       const uac2_stream_config_t *cfg);
    esp_err_t (*open)(uac2_stream_t *s);
    esp_err_t (*prepare)(uac2_stream_t *s);
    size_t    (*packetize)(uac2_stream_t *s, const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_size);
    void      (*close)(uac2_stream_t *s);
} uac2_format_driver_t;

extern const uac2_format_driver_t uac2_pcm_driver;
extern const uac2_format_driver_t uac2_dop_driver;
extern const uac2_format_driver_t uac2_native_dsd_driver;

/* ---- Quirk layer ---- */
#define UAC2_QUIRK_NATIVE_DSD     (1u << 0)
#define UAC2_QUIRK_ALTSET_DSD     (1u << 1)
#define UAC2_QUIRK_VENDOR_CONTROL (1u << 2)
#define UAC2_QUIRK_CLOCK_SPECIAL  (1u << 3)

typedef struct uac2_device uac2_device_t;

typedef struct {
    esp_err_t (*on_ready)(uac2_device_t *dev);
    esp_err_t (*on_stream_start)(uac2_device_t *dev, const uac2_stream_config_t *cfg);
    esp_err_t (*on_stream_stop)(uac2_device_t *dev);
} uac2_quirk_ops_t;

typedef struct {
    uint16_t vid, pid;
    uint32_t flags;
    const uac2_quirk_ops_t *ops;
} uac2_quirk_entry_t;

/* Lookup order: VID/PID -> family probe -> generic (quirk_none). Never NULL. */
const uac2_quirk_entry_t *uac2_quirk_find(uint16_t vid, uint16_t pid, const uint8_t *cfg_desc, size_t len);

/* ---- ISO engine (knows nothing about PCM/DSD) ---- */
#ifndef UAC2_NUM_TRANSFERS
#define UAC2_NUM_TRANSFERS 3
#endif

typedef struct {
    void   *transfers[UAC2_NUM_TRANSFERS]; /* usb_transfer_t* */
    uint8_t num_transfers;
    size_t  packet_bytes;
    volatile uint8_t running;
} uac2_iso_engine_t;

/* ---- Ring buffer ---- */
typedef struct {
    uint8_t *buffer;
    size_t size, read_pos, write_pos;
} audio_ring_t;

/* ---- Pure helpers (unit-tested) ---- */
/* Max bytes per USB interval: 1000 intervals/s on FS, 8000 on HS. */
uint32_t uac2_packet_bytes(uint32_t rate, uint8_t channels, uint8_t subslot, uint32_t intervals_per_sec);

esp_err_t audio_ring_init(audio_ring_t *r, size_t size);
void      audio_ring_deinit(audio_ring_t *r);
size_t    audio_ring_used(const audio_ring_t *r);
size_t    audio_ring_free(const audio_ring_t *r);
size_t    audio_ring_write(audio_ring_t *r, const uint8_t *src, size_t len);
size_t    audio_ring_read(audio_ring_t *r, uint8_t *dst, size_t len);

/* DoP: input per frame = 2 DSD bytes per channel [c0b1 c0b2 c1b1 c1b2 ...];
 * output = packed 24-bit LE per channel [b2 b1 marker]. Marker alternates 0x05/0xFA. */
typedef struct { uint8_t marker; } dop_context_t;
size_t uac2_dop_pack(dop_context_t *ctx, const uint8_t *dsd, size_t dsd_len,
                     uint8_t channels, uint8_t *dst, size_t dst_size);

/* DSD carrier rate: DoP PCM rate = dsd_rate*44100/16; native U32 rate = dsd_rate*44100/32 */
uint32_t uac2_dsd_dop_rate(uac2_dsd_rate_t r);
uint32_t uac2_dsd_native_rate(uac2_dsd_rate_t r);
/* ---- Ownership: device -> stream -> {format_ctx, iso, ring} ---- */
struct uac2_stream {
    const uac2_format_driver_t *fmt;
    uac2_stream_config_t cfg;
    const uac2_stream_cap_t *cap;
    void *format_ctx;
    uac2_iso_engine_t iso;
    audio_ring_t ring;
};

struct uac2_device {
    uac2_state_t state;
    uac2_device_caps_t caps;
    const uac2_quirk_entry_t *quirk;
    uac2_stream_t *stream;
    void *usb_dev;   /* usb_device_handle_t */
};

/* stream lifecycle */
esp_err_t uac2_stream_create(uac2_device_t *dev, const uac2_stream_config_t *cfg);
void      uac2_stream_destroy(uac2_device_t *dev);
void      uac2_device_destroy(uac2_device_t *dev);

/* parser: temp-malloc, produces compact caps, frees temp */
esp_err_t uac2_parse_config(const uint8_t *cfg_desc, size_t len, uac2_device_caps_t *out);
void      uac2_caps_free(uac2_device_caps_t *caps);

/* clock */
esp_err_t uac2_clock_set_rate(uac2_device_t *dev, uint8_t clock_id, uint32_t rate);
esp_err_t uac2_clock_get_rate(uac2_device_t *dev, uint8_t clock_id, uint32_t *rate);

/* port */
void *uac2_malloc(size_t size);       /* PSRAM-ok / general */
void *uac2_dma_malloc(size_t size);   /* internal DMA-capable */
void  uac2_free(void *p);
void  uac2_host_get_hw_caps(uac2_host_hw_caps_t *out);

#ifdef __cplusplus
}
#endif
