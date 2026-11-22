/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "quirk_entries.h"
#include "sdkconfig.h"

const euacx_driver_t *euacx_driver_find(uint16_t vid, uint16_t pid, uint16_t bcd)
{
    static const euacx_driver_t *const table[] = {
#ifdef CONFIG_EUACX_DRV_CX31993
        &euacx_quirk_cx31993,
#endif
#ifdef CONFIG_EUACX_DRV_ES9318
        &euacx_quirk_es9318,
        &euacx_quirk_es9318_hs,
#endif
        &euacx_drv_generic,
    };
    return euacx_driver_match(table, sizeof(table) / sizeof(table[0]), vid, pid, bcd);
}
