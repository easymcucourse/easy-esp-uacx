cmake_minimum_required(VERSION 3.16)

# easy-esp-uacx (own code) + upstream submodules under third_party/
set(TP "${CMAKE_CURRENT_LIST_DIR}/../../third_party")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/easy-esp-uacx"
    "${TP}/esp-uac2-host/components/uac2_host"        # uac2_cx upstream (Averyy)
    "${TP}/esp-usb/host/class/uac/usb_host_uac"# Espressif UAC1 host
    "${TP}/micro-flac")                               # esphome micro-flac

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_music_player)
