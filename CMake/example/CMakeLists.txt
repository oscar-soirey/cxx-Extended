cmake_minimum_required(VERSION 3.24)
project(CXXEExample LANGUAGES CXX)

cxxe_add_executable(CXXEExample
    src/main.cpp
    src/player.cppe
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
)
