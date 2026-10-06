# Public PCM runtime and private algorithm helpers used by regression tests.
set(srcs
    core/euacx_api.c
    core/euacx_manager.c
    core/euacx_bus.c
    core/euacx_callback.c
    core/euacx_parser.c
    core/euacx_caps.c
    core/euacx_control.c
    core/euacx_transport.c
    stream/euacx_stream.c
    stream/euacx_iso.c
    stream/euacx_feed.c
    stream/euacx_spsc.c
    core/euacx_selector.c
    stream/euacx_packetizer.c
    stream/euacx_ring.c
    format/euacx_pcm.c
    format/uac2_dop.c
    format/uac2_native_dsd.c
    drivers/euacx_drivers.c
    drivers/drv_generic.c
    port/euacx_mem.c)

if(CONFIG_EUACX_DRV_CX31993)
    list(APPEND srcs drivers/drv_cx31993.c)
endif()

# DoP/native DSD are private legacy helpers retained for tests only.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS private
                       REQUIRES esp_common
                       PRIV_REQUIRES usb freertos heap esp_rom)
