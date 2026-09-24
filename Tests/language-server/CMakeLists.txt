cmake_minimum_required(VERSION 3.16)
project(cxxe_language_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(cxxe-language-server cxxe_language_server.c)
target_compile_definitions(cxxe-language-server PRIVATE CXXE_LSP_BUILD=1)

if (MSVC)
    target_compile_options(cxxe-language-server PRIVATE /W4)
else()
    target_compile_options(cxxe-language-server PRIVATE
        -Wall
        -Wextra
        -Wno-unused-but-set-variable
    )
endif()
