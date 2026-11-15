cmake_minimum_required(VERSION 3.16)
project(cxxe_language_server VERSION 0.3.0 LANGUAGES C)

# The language server links the compiler modules (all but the CLI entry).
set(CXXE_COMPILER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../Compiler/Src")
file(GLOB CXXE_LS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c")
file(GLOB_RECURSE CXXE_COMPILER_SOURCES CONFIGURE_DEPENDS "${CXXE_COMPILER_SRC}/*.c")
list(FILTER CXXE_COMPILER_SOURCES EXCLUDE REGEX ".*/Cli/Main\\.c$")

add_executable(cxxe-language-server ${CXXE_LS_SOURCES} ${CXXE_COMPILER_SOURCES})
target_include_directories(cxxe-language-server PRIVATE "${CXXE_COMPILER_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/Src")
set_target_properties(cxxe-language-server PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(cxxe-language-server PRIVATE /W4)
else()
    target_compile_options(cxxe-language-server PRIVATE -Wall -Wextra)
endif()
