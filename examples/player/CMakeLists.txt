cmake_minimum_required(VERSION 3.16)

# Shared components (uac2_cx, uac1_host, esphome__micro-flac, uac2_host framework)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_music_player)
