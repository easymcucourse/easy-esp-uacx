file(GLOB TEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../components/easy-esp-uacx/test/*.c")
idf_component_register(SRCS "main.c" "../../common/uac2_playback_demo.c" ${TEST_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES easy-esp-uacx unity esp_system usb
                       WHOLE_ARCHIVE)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/easy-esp-uacx/private")
