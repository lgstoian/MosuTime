idf_component_register(
    SRCS "src/mosu_hal_board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        mosu_board
)
