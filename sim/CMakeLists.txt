cmake_minimum_required(VERSION 3.16)
project(mosu_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(mosu_sim
    mosu_sim.c
    ${ROOT}/components/mosu_core/src/mosu_battery.c
    ${ROOT}/components/mosu_core/src/mosu_digits.c
    ${ROOT}/components/mosu_core/src/mosu_phrase.c
    ${ROOT}/components/mosu_core/src/mosu_tap.c
    ${ROOT}/components/mosu_core/src/mosu_timefmt.c
    ${ROOT}/components/mosu_core/src/mosu_wrist.c
)

target_include_directories(mosu_sim PRIVATE ${ROOT}/components/mosu_core/include)

if(UNIX)
    target_link_libraries(mosu_sim m)
endif()

if(NOT MSVC)
    target_compile_options(mosu_sim PRIVATE -Wall -Wextra -Werror)
endif()
