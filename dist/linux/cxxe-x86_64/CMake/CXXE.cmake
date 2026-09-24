# CXXE.cmake
# CMake integration for CXXE (C++ Extended Compiler) 0.1.
#
# CXXE is a source-to-source frontend here:
#   .cppe -> cxxe roundtrip -> generated .cpp -> normal C++ compiler
#
# Public API:
#   cxxe_transform_sources(OUT_VAR SOURCES ... [OUTPUT_DIRECTORY dir]
#                           [INCLUDE_DIRS ...] [DEPENDS ...])
#   cxxe_add_executable(target [WIN32] [MACOSX_BUNDLE] [EXCLUDE_FROM_ALL]
#                        [OUTPUT_DIRECTORY dir]
#                        [INCLUDE_DIRS ...] [DEPENDS ...] sources...)
#   cxxe_add_library(target [STATIC|SHARED|MODULE] [EXCLUDE_FROM_ALL]
#                     [OUTPUT_DIRECTORY dir]
#                     [INCLUDE_DIRS ...] [DEPENDS ...] sources...)
#
# Configuration:
#   CXXE_EXECUTABLE       path to cxxe/cxxe.exe (auto-detected if omitted)
#
# The runtime is NOT searched for on disk. It is generated directly by
# cxxe.exe with `cxxe runtime export ...`.
#   CXXE_INCLUDE_HEADERS_AS_DEPENDENCIES
#                         when ON, .he/.h/.hpp found recursively in INCLUDE_DIRS
#                         are dependencies of .cppe transformations (default ON)

include_guard(GLOBAL)

set(CXXE_VERSION "0.1")

set(_CXXE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(CXXE_INCLUDE_HEADERS_AS_DEPENDENCIES
       "Treat .he/.h/.hpp files in CXXE include directories as transformation dependencies" ON)

function(_cxxe_require_executable)
    # Resolve the configured path before using it in any build rule.
    # Absolute paths are required for reliable Visual Studio/MSBuild custom
    # commands because their working directory is not guaranteed to be the
    # CMake source directory.
    if(CXXE_EXECUTABLE)
        if(IS_ABSOLUTE "${CXXE_EXECUTABLE}")
            set(_cxxe_absolute "${CXXE_EXECUTABLE}")
        else()
            get_filename_component(
                _cxxe_absolute
                "${CXXE_EXECUTABLE}"
                ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            )
        endif()

        set(
            CXXE_EXECUTABLE
            "${_cxxe_absolute}"
            CACHE FILEPATH
            "Path to the CXXE executable"
            FORCE
        )
    endif()

    # If no executable was explicitly provided, search PATH.
    if(NOT CXXE_EXECUTABLE)
        find_program(_CXXE_FOUND_EXECUTABLE NAMES cxxe cxxe.exe)
        if(_CXXE_FOUND_EXECUTABLE)
            get_filename_component(
                _CXXE_FOUND_EXECUTABLE
                "${_CXXE_FOUND_EXECUTABLE}"
                ABSOLUTE
            )
            set(
                CXXE_EXECUTABLE
                "${_CXXE_FOUND_EXECUTABLE}"
                CACHE FILEPATH
                "Path to the CXXE executable"
                FORCE
            )
        endif()
    endif()

    if(NOT CXXE_EXECUTABLE)
        message(
            FATAL_ERROR
            "CXXE executable not found. Set CXXE_EXECUTABLE to cxxe/cxxe.exe "
            "or put cxxe on PATH."
        )
    endif()

    if(NOT EXISTS "${CXXE_EXECUTABLE}")
        message(
            FATAL_ERROR
            "CXXE_EXECUTABLE does not exist: ${CXXE_EXECUTABLE}"
        )
    endif()
endfunction()

function(_cxxe_resolve_stde_directory)
    _cxxe_require_executable()

    # CXXE standard headers are installed relative to cxxe.exe: <exe>/stde.
    # Never base this path on the CMake module directory or the project source
    # directory, because the executable and its standard library may be
    # distributed independently of the CMake files.
    get_filename_component(_cxxe_exe_dir "${CXXE_EXECUTABLE}" DIRECTORY)
    get_filename_component(
        _cxxe_stde_dir
        "${_cxxe_exe_dir}/stde"
        ABSOLUTE
    )

    if(NOT IS_DIRECTORY "${_cxxe_stde_dir}")
        message(FATAL_ERROR
            "CXXE standard-header directory does not exist next to CXXE_EXECUTABLE: ${_cxxe_stde_dir}")
    endif()

    set(CXXE_STDE_DIR "${_cxxe_stde_dir}" CACHE PATH
        "Directory containing CXXE standard headers (relative to cxxe.exe)" FORCE)
endfunction()

function(_cxxe_prepare_runtime out_cpp out_hpp out_dir)
    _cxxe_require_executable()

    set(_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/cxxe/runtime")
    set(_runtime_cpp "${_runtime_dir}/cxxe_runtime.cpp")
    set(_runtime_hpp "${_runtime_dir}/cxxe_runtime.hpp")

    # The generated runtime rule is global to this CMake build directory.
    # This keeps multiple CXXE targets from registering the same OUTPUT twice.
    get_property(_prepared GLOBAL PROPERTY CXXE_RUNTIME_PREPARED)
    if(NOT _prepared)
        # cxxe.exe owns the runtime sources. They are generated in the build tree
        # and then added directly to every CXXE target as normal source files.
        add_custom_command(
            OUTPUT "${_runtime_cpp}" "${_runtime_hpp}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_runtime_dir}"
            COMMAND "${CXXE_EXECUTABLE}" runtime export "${_runtime_dir}"
            DEPENDS "${CXXE_EXECUTABLE}"
            COMMENT "CXXE: generating runtime sources"
            VERBATIM
        )

        set_source_files_properties(
            "${_runtime_cpp}" "${_runtime_hpp}"
            PROPERTIES GENERATED TRUE
        )
        set_property(GLOBAL PROPERTY CXXE_RUNTIME_PREPARED TRUE)
    endif()

    set(${out_cpp} "${_runtime_cpp}" PARENT_SCOPE)
    set(${out_hpp} "${_runtime_hpp}" PARENT_SCOPE)
    set(${out_dir} "${_runtime_dir}" PARENT_SCOPE)
endfunction()

function(cxxe_enable_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "cxxe_enable_runtime: target does not exist: ${target}")
    endif()

    _cxxe_prepare_runtime(_runtime_cpp _runtime_hpp _runtime_dir)

    # Deliberately do not use target_link_libraries(). The runtime is compiled
    # as part of the target itself.
    target_sources("${target}" PRIVATE
        "${_runtime_cpp}"
        "${_runtime_hpp}"
    )
    target_include_directories("${target}" PRIVATE "${_runtime_dir}")
    target_compile_features("${target}" PRIVATE cxx_std_17)
endfunction()

function(_cxxe_collect_header_dependencies out_var include_dirs)
    set(_deps)
    if(CXXE_INCLUDE_HEADERS_AS_DEPENDENCIES)
        foreach(_inc IN LISTS include_dirs)
            if(IS_DIRECTORY "${_inc}")
                file(GLOB_RECURSE _headers CONFIGURE_DEPENDS
                    "${_inc}/*.he"
                    "${_inc}/*.h"
                    "${_inc}/*.hh"
                    "${_inc}/*.hpp"
                    "${_inc}/*.hxx"
                    "${_inc}/*.cpp"
                    "${_inc}/*.cppe")
                list(APPEND _deps ${_headers})
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES _deps)
    set(${out_var} "${_deps}" PARENT_SCOPE)
endfunction()

function(_cxxe_collect_standard_sources out_var)
    _cxxe_resolve_stde_directory()

    set(_stde_src_dir "${CXXE_STDE_DIR}/src")
    if(NOT IS_DIRECTORY "${_stde_src_dir}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    # Every source belonging to CXXE's standard library is compiled as part
    # of each target that uses cxxe_add_executable()/cxxe_add_library().
    # .cppe files still go through cxxe_transform_sources(), while .cpp/.c
    # files are passed directly to the normal C/C++ build.
    file(GLOB_RECURSE _stde_sources CONFIGURE_DEPENDS
        "${_stde_src_dir}/*.cpp"
        "${_stde_src_dir}/*.c"
        "${_stde_src_dir}/*.cppe")

    list(SORT _stde_sources)
    set(${out_var} "${_stde_sources}" PARENT_SCOPE)
endfunction()

function(cxxe_transform_sources OUT_VAR)
    if(OUT_VAR STREQUAL "")
        message(FATAL_ERROR "cxxe_transform_sources: missing output variable")
    endif()

    set(options)
    set(oneValueArgs OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES INCLUDE_DIRS DEPENDS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG
        "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "cxxe_transform_sources: unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "cxxe_transform_sources: SOURCES is required")
    endif()

    _cxxe_require_executable()
    _cxxe_resolve_stde_directory()

    if(ARG_OUTPUT_DIRECTORY)
        set(_output_dir "${ARG_OUTPUT_DIRECTORY}")
    else()
        set(_output_dir "${CMAKE_CURRENT_BINARY_DIR}/cxxe")
    endif()
    get_filename_component(_output_dir "${_output_dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    file(MAKE_DIRECTORY "${_output_dir}")

    # User include directories are followed by CXXE's standard-header
    # directory. This makes includes such as:
    #
    #     #include <math.h>
    #
    # resolve automatically from ${CXXE_STDE_DIR} without requiring the
    # project to repeat the directory in every cxxe_* call.
    set(_include_args)
    set(_absolute_include_dirs)

    foreach(_inc IN LISTS ARG_INCLUDE_DIRS)
        get_filename_component(_inc "${_inc}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND _include_args -I "${_inc}")
        list(APPEND _absolute_include_dirs "${_inc}")
    endforeach()

    if(IS_DIRECTORY "${CXXE_STDE_DIR}")
        list(APPEND _include_args -I "${CXXE_STDE_DIR}")
        list(APPEND _absolute_include_dirs "${CXXE_STDE_DIR}")
    else()
        message(FATAL_ERROR
            "CXXE standard-header directory does not exist: ${CXXE_STDE_DIR}")
    endif()

    list(REMOVE_DUPLICATES _absolute_include_dirs)

    _cxxe_collect_header_dependencies(_header_deps "${_absolute_include_dirs}")

    set(_generated)
    foreach(_source IN LISTS ARG_SOURCES)
        get_filename_component(_abs_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_abs_source}")
            message(FATAL_ERROR "CXXE source does not exist: ${_source}")
        endif()

        get_filename_component(_ext "${_abs_source}" LAST_EXT)
        if(NOT _ext STREQUAL ".cppe")
            list(APPEND _generated "${_abs_source}")
            continue()
        endif()

        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_abs_source}")
        # Sources outside the current source tree are given a stable hashed
        # directory so ../ paths can never escape the generated directory.
        if(_rel MATCHES "^\\.\\.")
            string(MD5 _hash "${_abs_source}")
            get_filename_component(_base "${_abs_source}" NAME_WE)
            set(_rel "external/${_hash}/${_base}.cppe")
        endif()
        string(REGEX REPLACE "\\.cppe$" ".cpp" _rel_cpp "${_rel}")
        set(_output "${_output_dir}/${_rel_cpp}")
        get_filename_component(_output_parent "${_output}" DIRECTORY)

        add_custom_command(
            OUTPUT "${_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_parent}"
            COMMAND "${CXXE_EXECUTABLE}" ${_include_args} roundtrip
                    "${_abs_source}" "${_output}"
            DEPENDS "${_abs_source}" ${ARG_DEPENDS} ${_header_deps}
            COMMENT "CXXE ${_rel} -> ${_rel_cpp}"
            VERBATIM
        )

        set_source_files_properties("${_output}" PROPERTIES GENERATED TRUE)
        list(APPEND _generated "${_output}")
    endforeach()

    set(${OUT_VAR} "${_generated}" PARENT_SCOPE)
endfunction()

function(_cxxe_split_target_sources out_var args)
    # cxxe_add_* accepts ordinary positional source arguments while keeping
    # CXXE-specific options in keywords.
    set(${out_var} "${args}" PARENT_SCOPE)
endfunction()

function(cxxe_add_executable target)
    set(options WIN32 MACOSX_BUNDLE EXCLUDE_FROM_ALL)
    set(oneValueArgs OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES INCLUDE_DIRS DEPENDS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG
        "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(ARG_UNPARSED_ARGUMENTS)
        list(APPEND ARG_SOURCES ${ARG_UNPARSED_ARGUMENTS})
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "cxxe_add_executable(${target}): no source files")
    endif()

    _cxxe_collect_standard_sources(_cxxe_standard_sources)
    set(_cxxe_all_sources ${ARG_SOURCES} ${_cxxe_standard_sources})
    list(REMOVE_DUPLICATES _cxxe_all_sources)

    if(ARG_OUTPUT_DIRECTORY)
        set(_out_dir "${ARG_OUTPUT_DIRECTORY}")
    else()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/cxxe/${target}")
    endif()

    cxxe_transform_sources(_cxxe_sources
        SOURCES ${_cxxe_all_sources}
        INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
        DEPENDS ${ARG_DEPENDS}
        OUTPUT_DIRECTORY "${_out_dir}")

    set(_add_args)
    if(ARG_WIN32)
        list(APPEND _add_args WIN32)
    endif()
    if(ARG_MACOSX_BUNDLE)
        list(APPEND _add_args MACOSX_BUNDLE)
    endif()
    if(ARG_EXCLUDE_FROM_ALL)
        list(APPEND _add_args EXCLUDE_FROM_ALL)
    endif()
    add_executable("${target}" ${_add_args} ${_cxxe_sources})
    target_include_directories("${target}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CXXE_STDE_DIR}"
    )

    # The CXXE runtime is always compiled directly into CXXE targets.
    cxxe_enable_runtime("${target}")
endfunction()

function(cxxe_add_library target)
    set(options STATIC SHARED MODULE EXCLUDE_FROM_ALL)
    set(oneValueArgs OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES INCLUDE_DIRS DEPENDS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG
        "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(ARG_UNPARSED_ARGUMENTS)
        list(APPEND ARG_SOURCES ${ARG_UNPARSED_ARGUMENTS})
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "cxxe_add_library(${target}): no source files")
    endif()

    _cxxe_collect_standard_sources(_cxxe_standard_sources)
    set(_cxxe_all_sources ${ARG_SOURCES} ${_cxxe_standard_sources})
    list(REMOVE_DUPLICATES _cxxe_all_sources)

    set(_lib_type_count 0)
    if(ARG_STATIC)
        math(EXPR _lib_type_count "${_lib_type_count} + 1")
    endif()
    if(ARG_SHARED)
        math(EXPR _lib_type_count "${_lib_type_count} + 1")
    endif()
    if(ARG_MODULE)
        math(EXPR _lib_type_count "${_lib_type_count} + 1")
    endif()
    if(_lib_type_count GREATER 1)
        message(FATAL_ERROR
            "cxxe_add_library(${target}): STATIC, SHARED and MODULE are mutually exclusive")
    endif()

    if(ARG_OUTPUT_DIRECTORY)
        set(_out_dir "${ARG_OUTPUT_DIRECTORY}")
    else()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/cxxe/${target}")
    endif()

    cxxe_transform_sources(_cxxe_sources
        SOURCES ${_cxxe_all_sources}
        INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
        DEPENDS ${ARG_DEPENDS}
        OUTPUT_DIRECTORY "${_out_dir}")

    set(_type_args)
    if(ARG_STATIC)
        list(APPEND _type_args STATIC)
    endif()
    if(ARG_SHARED)
        list(APPEND _type_args SHARED)
    endif()
    if(ARG_MODULE)
        list(APPEND _type_args MODULE)
    endif()
    if(ARG_EXCLUDE_FROM_ALL)
        list(APPEND _type_args EXCLUDE_FROM_ALL)
    endif()
    add_library("${target}" ${_type_args} ${_cxxe_sources})
    target_include_directories("${target}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CXXE_STDE_DIR}"
    )

    # The CXXE runtime is always compiled directly into CXXE targets.
    cxxe_enable_runtime("${target}")
endfunction()

# Report the configured toolchain in the configure log once the module is used.
if(CXXE_EXECUTABLE)
    message(STATUS "CXXE: ${CXXE_EXECUTABLE}")
endif()
message(STATUS "CXXE: .cppe sources will be transformed to generated .cpp files")
if(CXXE_EXECUTABLE)
    get_filename_component(_CXXE_CONFIGURED_EXE_DIR "${CXXE_EXECUTABLE}" DIRECTORY)
    get_filename_component(_CXXE_CONFIGURED_STDE_DIR "${_CXXE_CONFIGURED_EXE_DIR}/stde" ABSOLUTE)
    message(STATUS "CXXE: standard headers: ${_CXXE_CONFIGURED_STDE_DIR}")
endif()
message(STATUS "CXXE: runtime is generated by cxxe.exe and compiled directly into CXXE targets")
