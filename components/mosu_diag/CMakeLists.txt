idf_component_register(
    SRCS "src/mosu_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mosu_audio
        mosu_board
        mosu_config
        mosu_core
        mosu_net
        mosu_tasks
        mosu_time
        mosu_ui
        app_update
        esp_driver_usb_serial_jtag
        esp_system
        freertos
        nvs_flash
)
