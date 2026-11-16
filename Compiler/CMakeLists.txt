cmake_minimum_required(VERSION 3.28)
project(atri_compiler VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(atri_frontend STATIC
    src/lexer.c
    src/parser.c
    src/diagnostic.c
    src/ir.c
    src/lower.c
    src/optimize.c
    src/backend_x64.c
)
target_include_directories(atri_frontend PUBLIC src)

add_executable(atri-parse src/main.c)
target_link_libraries(atri-parse PRIVATE atri_frontend)
add_executable(atri-cc src/main.c)
target_link_libraries(atri-cc PRIVATE atri_frontend)
target_compile_definitions(atri-cc PRIVATE ATRI_COMPILER_DRIVER)
if(WIN32)
    target_compile_definitions(atri-parse PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(atri-cc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(frontend-tests tests/frontend_tests.c)
    target_link_libraries(frontend-tests PRIVATE atri_frontend)
    add_test(NAME frontend COMMAND frontend-tests)
    add_executable(compiler-tests tests/compiler_tests.c)
    target_link_libraries(compiler-tests PRIVATE atri_frontend)
    add_test(NAME compiler COMMAND compiler-tests)
    add_test(NAME parse-example
        COMMAND atri-parse "${CMAKE_CURRENT_SOURCE_DIR}/examples/main.txt")
    set_tests_properties(parse-example PROPERTIES
        PASS_REGULAR_EXPRESSION "Function main -> i32")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        find_program(ATRI_TEST_CLANG NAMES clang)
        if(ATRI_TEST_CLANG)
            add_test(NAME native-integers COMMAND "${CMAKE_COMMAND}"
                "-DCOMPILER=$<TARGET_FILE:atri-cc>"
                "-DCLANG=${ATRI_TEST_CLANG}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-tests"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_tests.cmake")
        endif()
    endif()
endif()

set(atri_targets atri_frontend atri-parse atri-cc)
if(BUILD_TESTING)
    list(APPEND atri_targets frontend-tests compiler-tests)
endif()
foreach(atri_target IN LISTS atri_targets)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${atri_target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    elseif(MSVC)
        target_compile_options(${atri_target} PRIVATE /W4 /WX)
    endif()
endforeach()
