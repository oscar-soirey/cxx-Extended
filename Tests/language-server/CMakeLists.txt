cmake_minimum_required(VERSION 3.16)
project(cxxe_language_server_tests C)

# Builds the language server from Tools/Ls and runs the protocol tests:
#   cmake -S Tests/language-server -B build-ls && cmake --build build-ls && ctest --test-dir build-ls
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../Tools/Ls" ls)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME lsp_protocol
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_lsp_tests.py" $<TARGET_FILE:cxxe-language-server>)
endif()
