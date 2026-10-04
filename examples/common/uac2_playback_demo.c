/*
 * S3 (FS) / P4 (HS) USB Host UAC2 DAC demo.
 *  - installs USB Host, waits for a DAC, logs enumeration speed (HS/FS)
 *  - minimal UAC2 descriptor parse (clock source, PCM 2ch playback alts)
 *  - for each test rate: SET_CUR clock, verify GET_CUR, stream "Twinkle Twinkle"
 *    (4 bars) over ISO OUT.
 * Async feedback endpoint is NOT used (nominal-rate streaming), fine for a short test.
 */
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#define TAG "uac2_playback"
#include "sdkconfig.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"

#define MAX_CANDS        16
#define NUM_XFERS        4
#define PKTS_PER_XFER    8
#define NOTE_SEC         0.3f
#define AMPLITUDE        0.2f

typedef struct {
    uint8_t  ifnum, alt, ep, channels, subslot, bitres, binterval, terminal;
    uint16_t mps;           /* bytes per interval incl. high-bandwidth multiplier */
} cand_t;

typedef struct {
    uint8_t id, source;
    uint32_t mute_read, mute_write, volume_read, volume_write;
} feature_t;

typedef struct {
    uint8_t  ac_if, clock_id;
    cand_t   cands[MAX_CANDS];
    int      ncands;
    feature_t features[MAX_CANDS];
    int nfeatures;
    uint8_t source[256];
} dac_t;

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static SemaphoreHandle_t        s_new_dev, s_ctrl_done, s_play_done;
static uint8_t                  s_new_addr;
static volatile bool            s_gone;

/* ---------------- USB host plumbing ---------------- */
static void client_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = m->new_dev.address;
        xSemaphoreGive(s_new_dev);
    } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (m->dev_gone.dev_hdl == s_dev) s_gone = true;
        ESP_LOGW(TAG, "device gone");
    }
}
static void lib_task(void *a)
{
    while (1) {
        uint32_t f;
        usb_host_lib_handle_events(portMAX_DELAY, &f);
    }
}
static void client_task(void *a)
{
    while (1) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

/* ---------------- control transfers ---------------- */
static void ctrl_cb(usb_transfer_t *t) { xSemaphoreGive(s_ctrl_done); }

static esp_err_t ctrl(uint8_t reqtype, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
    usb_transfer_t *t;
    esp_err_t e = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &t);
    if (e != ESP_OK) return e;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = reqtype; s->bRequest = req; s->wValue = val; s->wIndex = idx; s->wLength = len;
    if (!(reqtype & 0x80) && len) memcpy(t->data_buffer + sizeof(*s), data, len);
    t->num_bytes = sizeof(*s) + len;
    t->device_handle = s_dev;
    t->bEndpointAddress = 0;
    t->callback = ctrl_cb;
    t->timeout_ms = 1000;
    e = usb_host_transfer_submit_control(s_client, t);
    if (e == ESP_OK) {
        xSemaphoreTake(s_ctrl_done, portMAX_DELAY);
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) e = ESP_FAIL;
        else if (t->actual_num_bytes < sizeof(*s) + len) e = ESP_ERR_INVALID_SIZE;
        else if ((reqtype & 0x80) && len) memcpy(data, t->data_buffer + sizeof(*s), len);
    }
    usb_host_transfer_free(t);
    return e;
}

static esp_err_t clock_set(const dac_t *d, uint32_t hz)
{
    uint8_t v[4] = { hz, hz >> 8, hz >> 16, hz >> 24 };
    return ctrl(0x21, 0x01, 0x0100, (d->clock_id << 8) | d->ac_if, v, 4);   /* SET_CUR, CS_SAM_FREQ */
}
static esp_err_t clock_get(const dac_t *d, uint32_t *hz)
{
    uint8_t v[4] = {0};
    esp_err_t e = ctrl(0xA1, 0x01, 0x0100, (d->clock_id << 8) | d->ac_if, v, 4);
    *hz = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
    return e;
}

/* Claim/release only manages host-side pipes; the class driver must send
 * the standard request to switch the actual device's alternate setting. */
static esp_err_t set_interface(uint8_t ifnum, uint8_t alt)
{
    esp_err_t e = ctrl(0x01, 0x0B, alt, ifnum, NULL, 0);
    uint8_t got = 0xFF;
    if (e == ESP_OK) e = ctrl(0x81, 0x0A, 0, ifnum, &got, 1);
    ESP_LOGI(TAG, "SET_INTERFACE if%u alt%u readback=%u: %s",
             ifnum, alt, got, esp_err_to_name(e));
    if (e == ESP_OK && got != alt) e = ESP_ERR_INVALID_RESPONSE;
    return e;
}

/* ---------------- descriptor parse ---------------- */
static void parse_cfg(const usb_config_desc_t *cfg, dac_t *d)
{
    memset(d, 0, sizeof(*d));
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    uint8_t cls = 0, sub = 0, proto = 0, ifn = 0, alt = 0;
    cand_t cur = {0};
    bool have_cur = false;
    bool have_fmt = false;

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t len = p[0], type = p[1];
        if (type == 4 && len >= 9) {
            ifn = p[2]; alt = p[3]; cls = p[5]; sub = p[6]; proto = p[7];
            have_cur = false; have_fmt = false;
            if (cls == 1 && sub == 1) d->ac_if = ifn;
            if (cls == 1 && sub == 2 && proto == 0x20 && alt > 0) {
                memset(&cur, 0, sizeof(cur));
                cur.ifnum = ifn; cur.alt = alt; have_cur = true;
            }
        } else if (type == 0x24 && cls == 1 && sub == 1 && proto == 0x20 && len >= 12 && p[2] == 0x06) {
            d->source[p[3]] = p[4];
            if (d->nfeatures < MAX_CANDS && (len - 6) % 4 == 0) {
                feature_t *fu = &d->features[d->nfeatures++];
                fu->id = p[3]; fu->source = p[4];
                for (unsigned ch = 0; ch < (len - 6) / 4 && ch < 32; ch++) {
                    const uint8_t *b = p + 5 + ch * 4;
                    uint32_t controls = b[0] | ((uint32_t)b[1] << 8) |
                                        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                    if (controls & 1) fu->mute_read |= 1u << ch;
                    if ((controls & 3) == 3) fu->mute_write |= 1u << ch;
                    if (controls & 4) fu->volume_read |= 1u << ch;
                    if ((controls & 12) == 12) fu->volume_write |= 1u << ch;
                }
            }
        } else if (type == 0x24 && cls == 1 && sub == 1 && proto == 0x20 && len >= 12 && p[2] == 0x03) {
            d->source[p[3]] = p[7]; /* Output Terminal's bSourceID */
        } else if (type == 0x24 && cls == 1 && sub == 1 && len >= 8 && p[2] == 0x0A && !d->clock_id) {
            d->clock_id = p[3];
        } else if (type == 0x24 && have_cur && cls == 1 && sub == 2) {
            if (len >= 16 && p[2] == 0x01) {            /* AS_GENERAL */
                bool pcm = p[5] == 1 && (p[6] & 1);
                cur.channels = p[10];
                cur.terminal = p[3];
                have_fmt = pcm;
            } else if (len >= 6 && p[2] == 0x02) {      /* FORMAT_TYPE I */
                cur.subslot = p[4]; cur.bitres = p[5];
            }
        } else if (type == 5 && have_cur && have_fmt && len >= 7) {
            uint8_t addr = p[2], attr = p[3];
            if (!(addr & 0x80) && (attr & 3) == 1 && ((attr >> 4) & 3) == 0) {  /* iso OUT data */
                uint16_t w = p[4] | (p[5] << 8);
                cur.ep = addr; cur.binterval = p[6];
                cur.mps = (w & 0x7FF) * (1 + ((w >> 11) & 3));
                if (cur.channels == 2 && cur.subslot >= 2 && cur.subslot <= 4 &&
                    cur.bitres > 0 && cur.bitres <= cur.subslot * 8 && cur.binterval >= 1 && cur.binterval <= 4 && d->ncands < MAX_CANDS)
                    d->cands[d->ncands++] = cur;
            }
        }
        p += len;
    }
    ESP_LOGI(TAG, "AC if=%u clock_id=%u, %d stereo PCM playback alt(s)", d->ac_if, d->clock_id, d->ncands);
    for (int i = 0; i < d->ncands; i++) {
        const cand_t *c = &d->cands[i];
        ESP_LOGI(TAG, "  if%u alt%u ep0x%02x %ubit/%uB mps=%u bInterval=%u",
                 c->ifnum, c->alt, c->ep, c->bitres, c->subslot, c->mps, c->binterval);
    }
}

/* The CX31993 S3 reference applies master unmute then L/R -10.5 dB.
 * Resolve the Feature Unit on this AS terminal's path rather than taking
 * the first unit (which could belong to the microphone). */
static const feature_t *playback_feature(const dac_t *d, const cand_t *c)
{
    uint8_t id = c->terminal;
    for (int hop = 0; id && hop < 256; hop++) {
        for (int i = 0; i < d->nfeatures; i++) {
            if (d->features[i].id == id || d->features[i].source == id)
                return &d->features[i];
        }
        id = d->source[id];
    }
    return NULL;
}

static bool apply_playback_controls(const dac_t *d, const cand_t *c)
{
    const feature_t *fu = playback_feature(d, c);
    if (!fu) {
        ESP_LOGI(TAG, "playback terminal=%u has no Feature Unit", c->terminal);
        return true;
    }
    ESP_LOGI(TAG, "playback Feature Unit=%u mute_write=0x%lx volume_write=0x%lx",
             fu->id, (unsigned long)fu->mute_write, (unsigned long)fu->volume_write);
    uint16_t entity = (fu->id << 8) | d->ac_if;
    for (unsigned ch = 0; ch <= c->channels; ch++) {
        if (!(fu->mute_write & (1u << ch))) continue;
        uint8_t mute = 0;
        esp_err_t e = ctrl(0x21, 1, 0x0100 | ch, entity, &mute, 1);
        if (e == ESP_OK && (fu->mute_read & (1u << ch)))
            e = ctrl(0xA1, 1, 0x0100 | ch, entity, &mute, 1);
        ESP_LOGI(TAG, "mute ch%u OFF/readback=%u: %s", ch, mute, esp_err_to_name(e));
        if (e != ESP_OK || mute) return false;
    }
    for (unsigned ch = 0; ch <= c->channels; ch++) {
        if (!(fu->volume_write & (1u << ch))) continue;
        /* 0xF580 = -10.5 dB, matching the supplied S3/USBPcap sequence. */
        uint8_t volume[2] = {0x80, 0xF5};
        esp_err_t e = ctrl(0x21, 1, 0x0200 | ch, entity, volume, 2);
        if (e == ESP_OK && (fu->volume_read & (1u << ch)))
            e = ctrl(0xA1, 1, 0x0200 | ch, entity, volume, 2);
        int16_t got = (int16_t)(volume[0] | (volume[1] << 8));
        ESP_LOGI(TAG, "volume ch%u target=-10.50 dB readback=%.2f dB: %s",
                 ch, got / 256.0f, esp_err_to_name(e));
        if (e != ESP_OK || got != -2688) return false;
    }
    return true;
}

/* ---------------- Twinkle generator ---------------- */
/* C C G G | A A G - | F F E E | D D C -   (4 bars). cont=1 means held, no re-attack */
static const float s_freq[16] = {261.63f,261.63f,392.00f,392.00f, 440.00f,440.00f,392.00f,392.00f,
                                 349.23f,349.23f,329.63f,329.63f, 293.66f,293.66f,261.63f,261.63f};
static const uint8_t s_cont[16] = {0,0,0,0, 0,0,0,1, 0,0,0,0, 0,0,0,1};

typedef struct {
    uint32_t rate, ips, acc;
    uint64_t idx;
    uint32_t note_len, total, tail;
    float    phase;
    uint8_t  ch, subslot, ep;
    bool     done;
    volatile int inflight;
    volatile bool stop;
    uint32_t errors, packets, bytes;
    uint32_t pkt_max_bytes;
} play_t;
static play_t s_p;

static inline void put_sample(uint8_t *o, int32_t v, uint8_t subslot)
{
    for (int k = 0; k < subslot; k++) o[k] = (uint8_t)((uint32_t)v >> (8 * (4 - subslot + k)));
}

static void gen_frames(uint8_t *out, uint32_t frames)
{
    play_t *p = &s_p;
    for (uint32_t f = 0; f < frames; f++) {
        int32_t v = 0;
        if (p->idx < p->total) {
            uint32_t slot = p->idx / p->note_len, pos = p->idx % p->note_len;
            float env = 1.0f;
            if (!s_cont[slot]) {
                float a = pos / (0.005f * p->rate);
                if (a < env) env = a;
            }
            bool next_cont = slot < 15 && s_cont[slot + 1];
            if (!next_cont) {
                float r = (p->note_len - pos) / (0.03f * p->rate);
                if (r < env) env = r;
            }
            if (!s_cont[slot] && pos == 0) p->phase = 0;
            v = (int32_t)(sinf(p->phase) * env * AMPLITUDE * 2147483647.0f);
            p->phase += 6.2831853f * s_freq[slot] / p->rate;
            if (p->phase > 6.2831853f) p->phase -= 6.2831853f;
        }
        p->idx++;
        for (int c = 0; c < p->ch; c++) put_sample(out + (f * p->ch + c) * p->subslot, v, p->subslot);
    }
    if (p->idx >= p->total + p->tail) p->done = true;
}

static void fill_xfer(usb_transfer_t *t)
{
    play_t *p = &s_p;
    uint32_t off = 0, fb = p->ch * p->subslot;
    for (int i = 0; i < PKTS_PER_XFER; i++) {
        p->acc += p->rate;
        uint32_t frames = p->acc / p->ips;
        p->acc %= p->ips;
        gen_frames(t->data_buffer + off, frames);
        t->isoc_packet_desc[i].num_bytes = frames * fb;
        off += frames * fb;
    }
    t->num_bytes = off;
}

static void iso_cb(usb_transfer_t *t)
{
    play_t *p = &s_p;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        p->errors++;
        p->stop = true;
    }
    for (int i = 0; i < t->num_isoc_packets; i++) {
        if (t->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
            p->errors++;
            p->stop = true;
        } else {
            p->packets++;
            p->bytes += t->isoc_packet_desc[i].actual_num_bytes;
        }
    }
    if (!p->done && !p->stop && !s_gone) {
        fill_xfer(t);
        if (usb_host_transfer_submit(t) == ESP_OK) return;
        p->errors++;
        p->stop = true;
    }
    if (--p->inflight == 0) xSemaphoreGive(s_play_done);
}

static bool play_one_rate( const cand_t *c, uint32_t rate, uint32_t ips)
{
    play_t *p = &s_p;
    while (xSemaphoreTake(s_play_done, 0) == pdTRUE) {}
    memset(p, 0, sizeof(*p));
    p->rate = rate; p->ips = ips; p->ch = c->channels; p->subslot = c->subslot; p->ep = c->ep;
    p->note_len = (uint32_t)(NOTE_SEC * rate);
    p->total = p->note_len * 16;
    p->tail = rate / 10;
    uint32_t max_pkt = (rate + ips - 1) / ips + 1;           /* frames, +1 for accumulator */
    size_t buf = (size_t)max_pkt * p->ch * p->subslot * PKTS_PER_XFER;

    bool ok = false;
    int64_t started = esp_timer_get_time();
    usb_transfer_t *xf[NUM_XFERS] = {0};
    for (int i = 0; i < NUM_XFERS; i++) {
        if (usb_host_transfer_alloc(buf, PKTS_PER_XFER, &xf[i]) != ESP_OK) { ESP_LOGE(TAG, "alloc fail"); goto out; }
        xf[i]->device_handle = s_dev;
        xf[i]->bEndpointAddress = c->ep;
        xf[i]->callback = iso_cb;
        xf[i]->timeout_ms = 1000;
        xf[i]->context = NULL;
    }
    /* prefill all, then submit */
    p->inflight = NUM_XFERS;
    for (int i = 0; i < NUM_XFERS; i++) fill_xfer(xf[i]);
    for (int i = 0; i < NUM_XFERS; i++) {
        esp_err_t e = usb_host_transfer_submit(xf[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "iso submit failed: %s", esp_err_to_name(e));
            p->stop = true;
            p->errors++;
            p->inflight -= NUM_XFERS - i;
            break;
        }
    }
    if (p->inflight > 0 && xSemaphoreTake(s_play_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "playback timeout; cancel and drain before freeing transfers");
        p->stop = true;
        p->errors++;
        usb_host_endpoint_halt(s_dev, c->ep);
        usb_host_endpoint_flush(s_dev, c->ep);
        xSemaphoreTake(s_play_done, portMAX_DELAY);
        if (!s_gone) usb_host_endpoint_clear(s_dev, c->ep);
    }
    ok = p->done && !p->stop && !s_gone && p->errors == 0;
    ESP_LOGI(TAG, "RESULT rate=%lu status=%s packets=%lu bytes=%lu errors=%lu elapsed_ms=%lld",
             (unsigned long)rate, ok ? "PASS" : "FAIL", (unsigned long)p->packets,
             (unsigned long)p->bytes, (unsigned long)p->errors,
             (long long)((esp_timer_get_time() - started) / 1000));
out:
    for (int i = 0; i < NUM_XFERS; i++) if (xf[i]) usb_host_transfer_free(xf[i]);
    return ok;
}

/* ---------------- test flow ---------------- */
static void run_dac(void)
{
    if (usb_host_device_open(s_client, s_new_addr, &s_dev) != ESP_OK) { ESP_LOGE(TAG, "open failed"); return; }
    usb_device_info_t info; usb_host_device_info(s_dev, &info);
    const usb_device_desc_t *dd; usb_host_get_device_descriptor(s_dev, &dd);
    const usb_config_desc_t *cfg; usb_host_get_active_config_descriptor(s_dev, &cfg);
    ESP_LOGI(TAG, "device VID:PID=%04x:%04x speed=%s", dd->idVendor, dd->idProduct,
             info.speed == USB_SPEED_HIGH ? "HIGH (HS)" : info.speed == USB_SPEED_FULL ? "FULL (FS)" : "LOW");

    static dac_t d;
    parse_cfg(cfg, &d);
    if (!d.ncands || !d.clock_id) { ESP_LOGE(TAG, "no usable UAC2 stereo PCM alt / clock"); goto close; }

    static const uint32_t rates[] = {44100, 48000, 96000, 192000};
    int claimed = -1;
    unsigned passed = 0, skipped = 0, failed = 0;
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]) && !s_gone; r++) {
        uint32_t rate = rates[r];
        /* choose alt: highest bitres <= 24 whose mps fits this rate */
        const cand_t *best = NULL;
        for (int i = 0; i < d.ncands; i++) {
            const cand_t *c = &d.cands[i];
            uint32_t ips = (info.speed == USB_SPEED_HIGH ? 8000u : 1000u) >> 0;
            ips /= (1u << (c->binterval - 1));
            uint32_t need = ((rate + ips - 1) / ips) * c->channels * c->subslot;
            if (need > c->mps) continue;
            if (!best || (c->bitres <= 24 && (best->bitres > 24 || c->bitres > best->bitres))) best = c;
        }
        if (!best) { ESP_LOGW(TAG, "%u Hz: no alt with enough bandwidth, skip", (unsigned)rate); skipped++; continue; }

        if (claimed >= 0) {
            esp_err_t stop_err = set_interface(claimed, 0);
            usb_host_interface_release(s_client, s_dev, claimed);
            claimed = -1;
            if (stop_err != ESP_OK) { failed++; break; }
        }
        esp_err_t claim_err = usb_host_interface_claim(s_client, s_dev, best->ifnum, 0);
        if (claim_err != ESP_OK) { failed++; continue; }   /* zero-bandwidth alt for clock change */
        if (set_interface(best->ifnum, 0) != ESP_OK) {
            usb_host_interface_release(s_client, s_dev, best->ifnum);
            failed++;
            continue;
        }
        if (!apply_playback_controls(&d, best)) {
            ESP_LOGE(TAG, "playback controls failed; do not report a silent stream as passing");
            usb_host_interface_release(s_client, s_dev, best->ifnum);
            failed++;
            continue;
        }
        esp_err_t e = clock_set(&d, rate);
        uint32_t got = 0;
        if (e == ESP_OK) e = clock_get(&d, &got);
        usb_host_interface_release(s_client, s_dev, best->ifnum);
        if (e != ESP_OK || got != rate) {
            ESP_LOGW(TAG, "%u Hz: clock set/get failed (%s, got %u), skip", (unsigned)rate, esp_err_to_name(e), (unsigned)got);
            skipped++;
            continue;
        }
        if (usb_host_interface_claim(s_client, s_dev, best->ifnum, best->alt) != ESP_OK) {
            ESP_LOGE(TAG, "claim if%u alt%u failed", best->ifnum, best->alt); failed++; continue;
        }
        claimed = best->ifnum;
        if (set_interface(best->ifnum, best->alt) != ESP_OK) {
            failed++;
            continue;
        }
        uint32_t ips = (info.speed == USB_SPEED_HIGH ? 8000u : 1000u) / (1u << (best->binterval - 1));
        ESP_LOGI(TAG, ">>> %u Hz  %ubit/%uB  alt%u  %u intervals/s : Twinkle Twinkle x4 bars",
                 (unsigned)rate, best->bitres, best->subslot, best->alt, (unsigned)ips);
        if (play_one_rate(best, rate, ips)) passed++; else failed++;
        ESP_LOGI(TAG, "<<< %u Hz done", (unsigned)rate);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "SUMMARY passed=%u skipped=%u failed=%u", passed, skipped, failed);
    if (claimed >= 0) {
        if (!s_gone) set_interface(claimed, 0);
        usb_host_interface_release(s_client, s_dev, claimed);
    }
close:
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    ESP_LOGI(TAG, "test finished");
}

void uac2_playback_demo_run(void)
{
    s_new_dev = xSemaphoreCreateBinary();
    s_ctrl_done = xSemaphoreCreateBinary();
    s_play_done = xSemaphoreCreateBinary();

    assert(s_new_dev && s_ctrl_done && s_play_done);
    usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1, .peripheral_map = 0 };
    ESP_ERROR_CHECK(usb_host_install(&hc));
    assert(xTaskCreate(lib_task, "usb_lib", 4096, NULL, 20, NULL) == pdPASS);
    usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
                                    .async = { .client_event_callback = client_cb, .callback_arg = NULL } };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_client));
    assert(xTaskCreate(client_task, "usb_cli", 8192, NULL, 19, NULL) == pdPASS);

    #if CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI(TAG, "waiting for USB DAC on P4 HS port (peripheral 0)...");
#else
    ESP_LOGI(TAG, "waiting for USB DAC on S3 FS port (D-=GPIO19, D+=GPIO20)...");
#endif
    while (1) {
        xSemaphoreTake(s_new_dev, portMAX_DELAY);
        s_gone = false;
        run_dac();
    }
}
