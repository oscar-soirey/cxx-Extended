# CXXEToolchain.cmake
#
# Use with:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/CXXEToolchain.cmake
#
# This file deliberately does NOT replace CMAKE_CXX_COMPILER.  CXXE is a
# source-to-source frontend; GCC/MSVC/Clang remains the actual C++ compiler.

cmake_minimum_required(VERSION 3.24)

set(CXXE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH
    "Directory containing CXXE.cmake")

if(NOT CXXE_EXECUTABLE AND DEFINED ENV{CXXE_EXECUTABLE})
    set(CXXE_EXECUTABLE "$ENV{CXXE_EXECUTABLE}" CACHE FILEPATH
        "Path to the CXXE executable")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CXXE_CMAKE_DIR}")
list(REMOVE_DUPLICATES CMAKE_MODULE_PATH)

# Automatically load CXXE.cmake when project() is evaluated.  This keeps a
# toolchain-only setup possible while leaving the native C++ compiler intact.
set(_cxxe_project_include "${CXXE_CMAKE_DIR}/CXXE.cmake")
if(CMAKE_PROJECT_TOP_LEVEL_INCLUDES)
    list(FIND CMAKE_PROJECT_TOP_LEVEL_INCLUDES "${_cxxe_project_include}" _cxxe_include_index)
    if(_cxxe_include_index EQUAL -1)
        list(PREPEND CMAKE_PROJECT_TOP_LEVEL_INCLUDES "${_cxxe_project_include}")
    endif()
else()
    set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES "${_cxxe_project_include}" CACHE STRING
        "Top-level files automatically included by CMake")
endif()

set(CXXE_TOOLCHAIN_ACTIVE ON CACHE BOOL
    "Indicates that the CXXE CMake toolchain is active")
