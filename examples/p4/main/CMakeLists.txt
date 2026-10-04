file(GLOB TEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../components/uac2_host/test/*.c")
idf_component_register(SRCS "main.c" ${TEST_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES uac2_host unity esp_system
                       WHOLE_ARCHIVE)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/uac2_host/private")
