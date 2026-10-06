/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "euacx_driver.h"
#include "sdkconfig.h"

const euacx_driver_t *euacx_driver_match(const euacx_driver_t *const *table,
                                       size_t count, uint16_t vid, uint16_t pid, uint16_t bcd)
{
    const euacx_driver_t *best = NULL;
    unsigned rank = 0;
    for (size_t i = 0; table && i < count; ++i) {
        const euacx_driver_t *d = table[i];
        if (!d || !d->vid || d->vid != vid) continue;
        if (euacx_driver_validate(d) != ESP_OK || (d->pid && d->pid != pid)) continue;
#ifndef CONFIG_EUACX_DRV_REPORTED
        if (d->reported) continue;
#endif
        const euacx_driver_params_t *p = d->params;
        bool bounded = p && (p->bcd_min || p->bcd_max);
        if (bounded && (bcd < p->bcd_min || bcd > p->bcd_max)) continue;
        unsigned candidate = (d->pid ? 4 : 2) + bounded;
        if (candidate > rank) { best = d; rank = candidate; }
    }
    return best ? best : &euacx_drv_generic;
}

const euacx_driver_t *euacx_driver_find(uint16_t vid, uint16_t pid, uint16_t bcd)
{
    static const euacx_driver_t *const table[] = {
#ifdef CONFIG_EUACX_DRV_CX31993
        &euacx_drv_cx31993,
#endif
        &euacx_drv_generic,
    };
    return euacx_driver_match(table, sizeof(table) / sizeof(table[0]), vid, pid, bcd);
}

static bool valid_pcm(const euacx_verified_pcm_t *pcm, uint8_t count)
{
    if (count && !pcm) return false;
    for (uint8_t i = 0; i < count; ++i) {
        const euacx_verified_pcm_t *p = &pcm[i];
        if ((p->bits != 16 && p->bits != 24 && p->bits != 32) ||
            p->subslot < 2 || p->subslot > 4 || p->subslot * 8u < p->bits ||
            !p->rates.num_rates || p->rates.num_rates > EUACX_MAX_RATES) return false;
        for (uint8_t r = 0; r < p->rates.num_rates; ++r) {
            if (!p->rates.rates[r] || (r && p->rates.rates[r] <= p->rates.rates[r - 1])) return false;
        }
    }
    return true;
}

esp_err_t euacx_driver_validate(const euacx_driver_t *driver)
{
    if (!driver || !driver->name || !driver->name[0]) return ESP_ERR_INVALID_ARG;
    uint32_t flags = driver->flags;
    const euacx_driver_params_t *p = driver->params;
    if (flags & ~0xffu) return ESP_ERR_INVALID_ARG;
    /* Control/stream exceptions must name an exact product. */
    if (flags && (!driver->vid || !driver->pid)) return ESP_ERR_INVALID_ARG;
    if (p && p->bcd_min > p->bcd_max) return ESP_ERR_INVALID_ARG;
    if ((flags & EUACX_DRV_VOL_RANGE) && (!p || p->vol_min >= p->vol_max ||
        p->vol_res <= 0 || ((int32_t)p->vol_max - p->vol_min) % p->vol_res)) return ESP_ERR_INVALID_ARG;
    if ((flags & EUACX_DRV_CTL_DELAY) && (!p || !p->ctl_delay_us || p->ctl_delay_us > 20000)) return ESP_ERR_INVALID_ARG;
    if ((flags & EUACX_DRV_IFACE_DELAY) && (!p || !p->iface_delay_ms || p->iface_delay_ms > 200)) return ESP_ERR_INVALID_ARG;
    if ((flags & EUACX_DRV_VOL_MIN_IS_MUTE) && (flags & EUACX_DRV_NO_HW_VOLUME)) return ESP_ERR_INVALID_ARG;
    const euacx_verified_caps_t *v = driver->verified;
    if (v && (!valid_pcm(v->fs, v->num_fs) || !valid_pcm(v->hs, v->num_hs))) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
