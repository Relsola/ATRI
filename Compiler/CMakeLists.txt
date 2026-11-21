cmake_minimum_required(VERSION 3.28)
project(atri_compiler VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(atri_frontend STATIC
    src/types.c
    src/modules.c
    src/meta.c
    src/source_map.c
    src/meta_syntax.c
    src/meta_ast.c
    src/meta_check.c
    src/lexer.c
    src/ir_verify.c
    src/parser.c
    src/diagnostic.c
    src/ir.c
    src/lower.c
    src/consteval.c
    src/vm_memory.c
    src/vm_bytecode.c
    src/optimize.c
    src/backend_x64.c
    src/backend_llvm.c
)
target_include_directories(atri_frontend PUBLIC src)
target_compile_definitions(atri_frontend PRIVATE ATRI_STDLIB_DIR="$<TARGET_FILE_DIR:atri-cc>/stdlib")

add_executable(atri-parse src/main.c src/meta_host.c src/clang_driver.c src/llvm_pass.c)
target_link_libraries(atri-parse PRIVATE atri_frontend ${CMAKE_DL_LIBS})
add_executable(atri-cc src/main.c src/meta_host.c src/clang_driver.c src/llvm_pass.c)
foreach(module compiler mem list string)
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:atri-cc>/stdlib/${module}.ar"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/${module}.ar")
endforeach()
target_link_libraries(atri-cc PRIVATE atri_frontend ${CMAKE_DL_LIBS})
target_compile_definitions(atri-cc PRIVATE ATRI_COMPILER_DRIVER)
if(WIN32)
    set_target_properties(atri-parse atri-cc PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(atri-parse PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(atri-cc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include(CTest)
if(BUILD_TESTING)
    add_library(atri_test_support STATIC tests/test_support.c)
    target_link_libraries(atri_test_support PUBLIC atri_frontend)
    target_include_directories(atri_test_support PUBLIC tests)
    add_executable(frontend-tests tests/frontend_tests.c)
    target_link_libraries(frontend-tests PRIVATE atri_frontend)
    add_test(NAME frontend COMMAND frontend-tests)
    add_executable(compiler-tests tests/compiler_tests.c)
    target_link_libraries(compiler-tests PRIVATE atri_test_support)
    add_test(NAME compiler COMMAND compiler-tests)
    add_executable(control-flow-tests tests/control_flow_tests.c)
    target_link_libraries(control-flow-tests PRIVATE atri_test_support)
    add_test(NAME control-flow COMMAND control-flow-tests)
    add_executable(optimization-tests tests/optimization_tests.c)
    target_link_libraries(optimization-tests PRIVATE atri_test_support)
    add_test(NAME optimization COMMAND optimization-tests)
    add_executable(verifier-tests tests/verifier_tests.c)
    target_link_libraries(verifier-tests PRIVATE atri_test_support)
    add_test(NAME ir-verifier COMMAND verifier-tests)
    set_tests_properties(optimization control-flow PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME parse-example
        COMMAND atri-parse "${CMAKE_CURRENT_SOURCE_DIR}/examples/main.ar")
    set_tests_properties(parse-example PROPERTIES
        PASS_REGULAR_EXPRESSION "Function main -> i32")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        find_program(ATRI_TEST_CLANG NAMES clang)
        if(ATRI_TEST_CLANG)
            add_executable(test-bindings tests/generate_bindings.c)
            target_link_libraries(test-bindings PRIVATE atri_test_support)
            add_test(NAME compiler-pipeline COMMAND "${CMAKE_COMMAND}"
                "-DCOMPILER=$<TARGET_FILE:atri-cc>"
                "-DCLANG=${ATRI_TEST_CLANG}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/pipeline-tests"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_tests.cmake")
            add_test(NAME native-execution COMMAND "${CMAKE_COMMAND}"
                "-DCOMPILER=$<TARGET_FILE:atri-cc>"
                "-DCLANG=${ATRI_TEST_CLANG}"
                "-DBINDINGS_GENERATOR=$<TARGET_FILE:test-bindings>"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-tests"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_tests.cmake")
        endif()
    endif()
endif()

set(atri_targets atri_frontend atri-parse atri-cc)
if(BUILD_TESTING)
    list(APPEND atri_targets atri_test_support frontend-tests compiler-tests control-flow-tests optimization-tests verifier-tests)
    if(TARGET test-bindings)
        list(APPEND atri_targets test-bindings)
    endif()
endif()
foreach(atri_target IN LISTS atri_targets)
    if(WIN32)
        target_compile_definitions(${atri_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${atri_target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    elseif(MSVC)
        target_compile_options(${atri_target} PRIVATE /W4 /WX)
    endif()
endforeach()
