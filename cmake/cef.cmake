# CEF dependency setup for PrismaUI.
# This module intentionally makes CEF available to the build without changing
# PrismaUI's plugin runtime startup path.

set(PRISMAUI_CEF_ARCHIVE
    "${CMAKE_SOURCE_DIR}/external/cef_binary_147.0.14+g76d2442+chromium-147.0.7727.138_windows64.tar.bz2"
    CACHE FILEPATH "Path to the CEF binary distribution archive")

if(DEFINED BUILD_ROOT)
    set(_PRISMAUI_CEF_DEFAULT_EXTRACT_DIR "${BUILD_ROOT}/external_builds/cef")
else()
    set(_PRISMAUI_CEF_DEFAULT_EXTRACT_DIR "${CMAKE_BINARY_DIR}/external_builds/cef")
endif()

set(PRISMAUI_CEF_EXTRACT_DIR
    "${_PRISMAUI_CEF_DEFAULT_EXTRACT_DIR}"
    CACHE PATH "Directory used for the extracted CEF binary distribution")

get_filename_component(PRISMAUI_CEF_ARCHIVE "${PRISMAUI_CEF_ARCHIVE}" ABSOLUTE)
get_filename_component(PRISMAUI_CEF_EXTRACT_DIR "${PRISMAUI_CEF_EXTRACT_DIR}" ABSOLUTE)

if(NOT EXISTS "${PRISMAUI_CEF_ARCHIVE}")
    message(FATAL_ERROR
        "CEF binary archive not found.\n"
        "Expected: ${PRISMAUI_CEF_ARCHIVE}\n"
        "Place cef_binary_147.0.14+g76d2442+chromium-147.0.7727.138_windows64.tar.bz2 in the external directory "
        "or configure PRISMAUI_CEF_ARCHIVE to a local CEF binary archive.")
endif()

function(_prismaui_find_cef_root out_var search_dir)
    set(_result "")

    if(EXISTS "${search_dir}/cmake/FindCEF.cmake"
       AND EXISTS "${search_dir}/include/cef_app.h"
       AND EXISTS "${search_dir}/libcef_dll/CMakeLists.txt")
        set(_result "${search_dir}")
    else()
        file(GLOB _cef_root_candidates LIST_DIRECTORIES true "${search_dir}/*")
        foreach(_candidate IN LISTS _cef_root_candidates)
            if(IS_DIRECTORY "${_candidate}"
               AND EXISTS "${_candidate}/cmake/FindCEF.cmake"
               AND EXISTS "${_candidate}/include/cef_app.h"
               AND EXISTS "${_candidate}/libcef_dll/CMakeLists.txt")
                set(_result "${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${PRISMAUI_CEF_EXTRACT_DIR}")
_prismaui_find_cef_root(_PRISMAUI_CEF_ROOT "${PRISMAUI_CEF_EXTRACT_DIR}")

if(NOT _PRISMAUI_CEF_ROOT)
    message(STATUS "Extracting CEF archive: ${PRISMAUI_CEF_ARCHIVE}")
    file(ARCHIVE_EXTRACT
        INPUT "${PRISMAUI_CEF_ARCHIVE}"
        DESTINATION "${PRISMAUI_CEF_EXTRACT_DIR}")
    _prismaui_find_cef_root(_PRISMAUI_CEF_ROOT "${PRISMAUI_CEF_EXTRACT_DIR}")
endif()

if(NOT _PRISMAUI_CEF_ROOT)
    message(FATAL_ERROR
        "CEF extraction did not produce an expected binary distribution root under: ${PRISMAUI_CEF_EXTRACT_DIR}\n"
        "Required files: cmake/FindCEF.cmake, include/cef_app.h, libcef_dll/CMakeLists.txt")
endif()

set(PRISMAUI_CEF_ROOT "${_PRISMAUI_CEF_ROOT}" CACHE PATH "Extracted CEF binary distribution root" FORCE)
set(CEF_ROOT "${PRISMAUI_CEF_ROOT}")

set(_PRISMAUI_CEF_REQUIRED_PATHS
    "${CEF_ROOT}/cmake/FindCEF.cmake"
    "${CEF_ROOT}/include/cef_app.h"
    "${CEF_ROOT}/libcef_dll/CMakeLists.txt"
    "${CEF_ROOT}/Debug/libcef.lib"
    "${CEF_ROOT}/Release/libcef.lib"
    "${CEF_ROOT}/Resources/locales")

foreach(_required_path IN LISTS _PRISMAUI_CEF_REQUIRED_PATHS)
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "CEF package validation failed; missing required path: ${_required_path}")
    endif()
endforeach()

message(STATUS "PrismaUI CEF archive: ${PRISMAUI_CEF_ARCHIVE}")
message(STATUS "PrismaUI CEF root: ${CEF_ROOT}")

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")

# PrismaUI will run CEF unsandboxed when the runtime migration reaches initialization.
# Keep this explicit so the subprocess target does not require bootstrap/sandbox wiring.
set(USE_SANDBOX OFF CACHE BOOL "Disable CEF sandbox for PrismaUI" FORCE)

# The upstream CEF CMake files clear global CMAKE_CXX_FLAGS for Ninja builds.
# Preserve PrismaUI's existing compiler flag configuration and apply CEF flags only
# through CEF target macros/functions below.
set(_PRISMAUI_CEF_SAVED_FLAG_VARS
    CMAKE_C_FLAGS
    CMAKE_C_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_RELEASE)

foreach(_flag_var IN LISTS _PRISMAUI_CEF_SAVED_FLAG_VARS)
    set(_PRISMAUI_CEF_SAVED_${_flag_var} "${${_flag_var}}")
endforeach()

find_package(CEF REQUIRED)

foreach(_flag_var IN LISTS _PRISMAUI_CEF_SAVED_FLAG_VARS)
    set(${_flag_var} "${_PRISMAUI_CEF_SAVED_${_flag_var}}")
endforeach()

if(NOT TARGET libcef_dll_wrapper)
    set(_PRISMAUI_CEF_SAVED_CMAKE_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 20)
    add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/external_builds/libcef_dll_wrapper")
    set(CMAKE_CXX_STANDARD "${_PRISMAUI_CEF_SAVED_CMAKE_CXX_STANDARD}")
endif()

if(NOT TARGET libcef_lib)
    ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

message(STATUS "PrismaUI CEF binary dir: ${CEF_BINARY_DIR}")
message(STATUS "PrismaUI CEF resource dir: ${CEF_RESOURCE_DIR}")

function(prismaui_configure_cef_executable target_name)
    SET_EXECUTABLE_TARGET_PROPERTIES(${target_name})
    target_include_directories(${target_name}
        PRIVATE
        "${CEF_ROOT}"
        "${CEF_ROOT}/include")
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

function(_prismaui_copy_cef_single_file target_name source_file target_file)
    string(FIND "${source_file}" "$<CONFIGURATION>" _configuration_pos)
    if(NOT _configuration_pos EQUAL -1)
        string(REPLACE "$<CONFIGURATION>" "Release" _existing_source_file "${source_file}")
        if(NOT EXISTS "${_existing_source_file}")
            string(REPLACE "$<CONFIGURATION>" "Debug" _existing_source_file "${source_file}")
        endif()
    else()
        set(_existing_source_file "${source_file}")
    endif()

    get_filename_component(_target_parent "${target_file}" DIRECTORY)

    if(IS_DIRECTORY "${_existing_source_file}")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${target_file}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${source_file}" "${target_file}"
            VERBATIM)
    else()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_target_parent}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source_file}" "${target_file}"
            VERBATIM)
    endif()
endfunction()

function(prismaui_copy_cef_runtime target_name target_dir)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${target_dir}"
        VERBATIM)

    foreach(_cef_binary_file IN LISTS CEF_BINARY_FILES)
        get_filename_component(_cef_binary_name "${_cef_binary_file}" NAME)
        _prismaui_copy_cef_single_file(
            ${target_name}
            "${CEF_BINARY_DIR}/${_cef_binary_file}"
            "${target_dir}/${_cef_binary_name}")
    endforeach()

    foreach(_cef_resource_file IN LISTS CEF_RESOURCE_FILES)
        get_filename_component(_cef_resource_name "${_cef_resource_file}" NAME)
        _prismaui_copy_cef_single_file(
            ${target_name}
            "${CEF_RESOURCE_DIR}/${_cef_resource_file}"
            "${target_dir}/${_cef_resource_name}")
    endforeach()
endfunction()
