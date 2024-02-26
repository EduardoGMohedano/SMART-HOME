idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")


set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../WEB_UI")
if(EXISTS ${WEB_SRC_DIR}/)
    spiffs_create_partition_image(www ${WEB_SRC_DIR}/ FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${WEB_SRC_DIR}/ doesn't exit. Please make sure WEB files are in ${WEB_SRC_DIR}")
endif()