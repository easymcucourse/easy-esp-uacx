#include "uac2_internal.h"

static const uac2_quirk_entry_t s_generic = { 0, 0, 0, NULL };
const uac2_quirk_entry_t *uac2_quirk_generic(void) { return &s_generic; }
