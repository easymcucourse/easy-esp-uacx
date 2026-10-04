#include <stdlib.h>
#include "esp_heap_caps.h"
#include "uac2_internal.h"

void *uac2_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_8BIT); }
void *uac2_dma_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }
void  uac2_free(void *p) { heap_caps_free(p); }
