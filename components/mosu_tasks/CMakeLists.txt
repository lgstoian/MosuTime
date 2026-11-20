idf_component_register(
    SRCS "src/mosu_tasks.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
        heap
)
