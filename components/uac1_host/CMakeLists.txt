idf_component_register(SRCS "uac_host.c" "uac_descriptors.c"
                       INCLUDE_DIRS "include"
                       REQUIRES usb
                       PRIV_REQUIRES esp_ringbuf esp_timer)

# Source-only Espressif UAC1 reference: supply the Kconfig defaults it expects.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_UAC_FREQ_NUM_MAX=16
    CONFIG_UAC_DEV_ADDR_LIST_MAX=8
    CONFIG_UAC_RINGBUF_SAFE_DELETE_WAITING_MS=100
    CONFIG_UAC_NUM_ISOC_URBS=4
    CONFIG_UAC_NUM_PACKETS_PER_URB=1
    USB_HOST_UAC_VER_MAJOR=1
    USB_HOST_UAC_VER_MINOR=3
    USB_HOST_UAC_VER_PATCH=3
)
