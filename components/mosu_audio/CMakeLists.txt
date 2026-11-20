idf_component_register(
    SRCS "src/mosu_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mosu_board
        mosu_config
        mosu_core
        mosu_tasks
        esp_pm
        freertos
)
