cmake_minimum_required(VERSION 3.16)

# P4 tests only need the easy-esp-uacx framework
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/easy-esp-uacx")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uac2_example_p4)
