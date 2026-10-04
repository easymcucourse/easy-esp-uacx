set(srcs)
file(GLOB core_srcs core/*.c)
file(GLOB stream_srcs stream/*.c)
file(GLOB port_srcs port/*.c)
list(APPEND srcs ${core_srcs} ${stream_srcs} ${port_srcs} quirks/quirk_none.c quirks/quirk_table.c)

list(APPEND srcs format/uac2_pcm.c)
if(CONFIG_UAC2_DOP)
    list(APPEND srcs format/uac2_dop.c)
endif()
if(CONFIG_UAC2_NATIVE_DSD)
    list(APPEND srcs format/uac2_native_dsd.c)
endif()
if(CONFIG_UAC2_QUIRK_XMOS)
    list(APPEND srcs quirks/quirk_xmos.c)
endif()
if(CONFIG_UAC2_QUIRK_CMEDIA)
    list(APPEND srcs quirks/quirk_cmedia.c)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS private
                       REQUIRES usb esp_timer
                       PRIV_REQUIRES freertos heap)

