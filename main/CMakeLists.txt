file(GLOB_RECURSE LV_DEMOS_SOURCES ./*.c)

idf_component_register(
    SRC_DIRS
        "."
        "app"
        "gui"
        "gui/font"
        "gui/image"

    INCLUDE_DIRS
        "."
        "gui"
        "app"
        )

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")

target_compile_definitions(${COMPONENT_TARGET} PRIVATE "")

set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE)

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
