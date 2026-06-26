idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES display button mic net stt nvs_flash)

# Pack the project's fonts/ folder into the "storage" LittleFS partition and flash it.
littlefs_create_partition_image(storage "${CMAKE_CURRENT_SOURCE_DIR}/../fonts" FLASH_IN_PROJECT)
