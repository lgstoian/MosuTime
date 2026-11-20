idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        mosu_audio
        mosu_board
        mosu_config
        mosu_core
        mosu_diag
        mosu_net
        mosu_policy
        mosu_power
        mosu_session
        mosu_time
        mosu_ui
        esp_lvgl_port
        lvgl
)
