idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "test.mp3"
                       REQUIRES easy-esp-uacx esp_audio_codec)
