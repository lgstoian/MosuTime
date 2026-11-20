idf_component_register(
    SRCS "src/mosu_power.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_pm
        mosu_config
        mosu_hal
)
