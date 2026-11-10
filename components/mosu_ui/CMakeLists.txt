idf_component_register(
    SRCS "src/mosu_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mosu_config
        mosu_core
        lvgl
        esp_lvgl_port
)
