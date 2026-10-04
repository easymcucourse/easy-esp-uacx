cmake_minimum_required(VERSION 3.16)

# uac2_cx is esp32s3-only; P4 tests only need the uac2_host framework
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/uac2_host")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uac2_example_p4)
