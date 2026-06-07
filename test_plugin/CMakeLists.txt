cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CMAKE_COMPILE_JOBS)
    set(CMAKE_COMPILE_JOBS 8)
endif()

if(NOT DEFINED CMAKE_LINK_JOBS)
    set(CMAKE_LINK_JOBS 2)
endif()

set_property(GLOBAL PROPERTY JOB_POOLS "compile=${CMAKE_COMPILE_JOBS}" "link=${CMAKE_LINK_JOBS}")
set(CMAKE_JOB_POOL_COMPILE compile)
set(CMAKE_JOB_POOL_LINK link)

project(PrismaUITest VERSION 1.0.0 LANGUAGES CXX)

get_filename_component(PRISMAUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(COMMONLIBSSE_ROOT "${PRISMAUI_ROOT}/external/commonlibsse-ng")
set(COMMONLIBSSE_BUILD_DIR "${CMAKE_BINARY_DIR}/external_builds/CommonLibSSE")

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    string(REGEX REPLACE "/EH[scra-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(APPEND CMAKE_CXX_FLAGS " /permissive- /Zc:preprocessor /MP /W4 /utf-8")

    foreach(flag_var CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "/EH[scra-z]+" "" ${flag_var} "${${flag_var}}")
        string(APPEND ${flag_var} " /EHa /bigobj")
        string(REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
    endforeach()

    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
add_subdirectory("${COMMONLIBSSE_ROOT}" "${COMMONLIBSSE_BUILD_DIR}" EXCLUDE_FROM_ALL)
include("${COMMONLIBSSE_ROOT}/cmake/CommonLibSSE.cmake")

file(READ "${COMMONLIBSSE_ROOT}/CMakeLists.txt" _commonlib_cmakelists_content)
string(REGEX MATCH "VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${_commonlib_cmakelists_content}")
set(COMMONLIBSSE_VERSION "${CMAKE_MATCH_1}" CACHE STRING "CommonLibSSE-NG version" FORCE)
add_compile_definitions(COMMONLIBSSE_VERSION="${COMMONLIBSSE_VERSION}")

find_package(spdlog REQUIRED CONFIG)

file(GLOB_RECURSE PRISMAUI_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "Source Files"
    FILES ${PRISMAUI_TEST_SOURCES}
)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "PrismaUITest"
    AUTHOR "PrismaUI"
    EMAIL ""
    USE_ADDRESS_LIBRARY
    MINIMUM_SKSE_VERSION "2.0.1.04"
    VERSION ${PROJECT_VERSION}
    SOURCES ${PRISMAUI_TEST_SOURCES}
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${PRISMAUI_ROOT}/src"
        "${COMMONLIBSSE_ROOT}/include"
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        spdlog::spdlog
)

target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

set(TEST_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/PrismaUITest")
set(TEST_DIST_SKSE_DIR "${TEST_DIST_DIR}/SKSE/plugins")
set(TEST_DIST_PRISMAUI_DIR "${TEST_DIST_DIR}/PrismaUI")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${TEST_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_DIST_SKSE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_DIST_PRISMAUI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${TEST_DIST_SKSE_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_PDB_FILE:${PROJECT_NAME}>>,copy,true>
        "$<$<BOOL:$<TARGET_PDB_FILE:${PROJECT_NAME}>>:$<TARGET_PDB_FILE:${PROJECT_NAME}>>"
        "${TEST_DIST_SKSE_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/PrismaUI" "${TEST_DIST_PRISMAUI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Created PrismaUI API test plugin package: ${TEST_DIST_DIR}"
)
