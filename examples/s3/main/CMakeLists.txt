set(TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/easy-esp-uacx/test")
set(TEST_SRCS "${TEST_DIR}/test_easy_uacx.c" "${TEST_DIR}/test_pcm_model.c" "${TEST_DIR}/test_runtime_lifecycle.c" "${TEST_DIR}/test_runtime_stream.c")
idf_component_register(SRCS "main.c" "../../common/easy_uacx_demo.c" "../../common/easy_uacx_validation.c" ${TEST_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES easy-esp-uacx unity esp_system usb esp_timer
                       WHOLE_ARCHIVE)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/easy-esp-uacx/private")
