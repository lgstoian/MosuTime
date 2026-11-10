cmake_minimum_required(VERSION 3.16)
project(mosu_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(mosu_tests
    test_core.c
    ${ROOT}/components/mosu_core/src/mosu_battery.c
    ${ROOT}/components/mosu_core/src/mosu_digits.c
    ${ROOT}/components/mosu_core/src/mosu_phrase.c
    ${ROOT}/components/mosu_core/src/mosu_tap.c
    ${ROOT}/components/mosu_core/src/mosu_timefmt.c
    ${ROOT}/components/mosu_core/src/mosu_wrist.c
    ${ROOT}/components/mosu_config/src/mosu_config.c
)

target_include_directories(mosu_tests PRIVATE
    ${ROOT}/components/mosu_core/include
    ${ROOT}/components/mosu_config/include
)

if(UNIX)
    target_link_libraries(mosu_tests m)
endif()

if(MSVC)
    target_compile_options(mosu_tests PRIVATE /W4)
else()
    target_compile_options(mosu_tests PRIVATE -Wall -Wextra -Werror)
endif()
