idf_component_register(
    SRCS
        "src/mosu_config.c"
        "src/mosu_config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)

# The host test build uses src/mosu_config.c directly and never compiles the
# NVS cache above.
