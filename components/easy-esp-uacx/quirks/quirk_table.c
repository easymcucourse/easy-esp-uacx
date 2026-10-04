#include "uac2_internal.h"

const uac2_quirk_entry_t *uac2_quirk_generic(void);

static const uac2_quirk_entry_t s_table[] = {
#ifdef CONFIG_UAC2_QUIRK_CX31993
    { 0x06CB, 0x1594, UAC2_QUIRK_VENDOR_CONTROL, &uac2_quirk_cx31993_ops },  /* Synaptics/Conexant CX31993 */
    { 0x0572, 0x1B08, UAC2_QUIRK_VENDOR_CONTROL, &uac2_quirk_cx31993_ops },
    { 0x0572, 0x1B09, UAC2_QUIRK_VENDOR_CONTROL, &uac2_quirk_cx31993_ops },
#endif
    { 0x20B1, 0x0000, UAC2_QUIRK_NATIVE_DSD, NULL },  /* XMOS family placeholder: pid 0 = any */
};

const uac2_quirk_entry_t *uac2_quirk_find(uint16_t vid, uint16_t pid, const uint8_t *cfg_desc, size_t len)
{
    (void)cfg_desc; (void)len;
    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]); i++) {
        if (s_table[i].vid == vid && (s_table[i].pid == pid || s_table[i].pid == 0)) return &s_table[i];
    }
    return uac2_quirk_generic();
}
