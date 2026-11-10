cmake_minimum_required(VERSION 3.16)
project(mosu_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(mosu_tests
    test_core.c
    test_session.c
    test_config.c
    test_bmp.c
    test_policy.c
    ${ROOT}/components/mosu_core/src/mosu_battery.c
    ${ROOT}/components/mosu_core/src/mosu_bmp.c
    ${ROOT}/components/mosu_core/src/mosu_digits.c
    ${ROOT}/components/mosu_core/src/mosu_phrase.c
    ${ROOT}/components/mosu_core/src/mosu_tap.c
    ${ROOT}/components/mosu_core/src/mosu_timefmt.c
    ${ROOT}/components/mosu_core/src/mosu_wrist.c
    ${ROOT}/components/mosu_config/src/mosu_config.c
    ${ROOT}/components/mosu_session/src/mosu_session.c
    ${ROOT}/components/mosu_policy/src/mosu_policy.c
)

target_include_directories(mosu_tests PRIVATE
    ${ROOT}/components/mosu_core/include
    ${ROOT}/components/mosu_config/include
    ${ROOT}/components/mosu_session/include
    ${ROOT}/components/mosu_policy/include
)

# The config suite reads the shipped card image, whichever directory the
# build happens to live in.
target_compile_definitions(mosu_tests PRIVATE
    MOSU_SOURCE_DIR="${ROOT}"
)

if(UNIX)
    target_link_libraries(mosu_tests m)
endif()

if(MSVC)
    target_compile_options(mosu_tests PRIVATE /W4)
else()
    target_compile_options(mosu_tests PRIVATE -Wall -Wextra -Werror)
endif()
