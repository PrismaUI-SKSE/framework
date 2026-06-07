cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Limit build parallelism (configurable via -DCMAKE_COMPILE_JOBS and -DCMAKE_LINK_JOBS)
if(NOT DEFINED CMAKE_COMPILE_JOBS)
    set(CMAKE_COMPILE_JOBS 8)
endif()

if(NOT DEFINED CMAKE_LINK_JOBS)
    set(CMAKE_LINK_JOBS 2)
endif()

set_property(GLOBAL PROPERTY JOB_POOLS "compile=${CMAKE_COMPILE_JOBS}" "link=${CMAKE_LINK_JOBS}")
set(CMAKE_JOB_POOL_COMPILE compile)
set(CMAKE_JOB_POOL_LINK link)
set(CMAKE_INSTALL_MESSAGE LAZY)

project(PrismaUI VERSION 2.0.0 LANGUAGES CXX)

# Set build root for external builds
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
set(BUILD_ROOT "${CMAKE_SOURCE_DIR}/build")

set(PRISMAUI_SHELL_APP_DIR "${CMAKE_SOURCE_DIR}/shell/app")
set(PRISMAUI_SHELL_DIST_DIR "${CMAKE_SOURCE_DIR}/shell/dist")
set(PRISMAUI_BOOTSTRAP_JS "${CMAKE_SOURCE_DIR}/shell/dist-bootstrap/bootstrap.js")
set(PRISMAUI_BOOTSTRAP_HEADER "${CMAKE_BINARY_DIR}/generated/PrismaBootstrapScript.generated.h")
find_program(PRISMAUI_NPM_EXECUTABLE NAMES npm npm.cmd REQUIRED DOC "npm executable required to build the PrismaUI CEF shell")

file(GLOB PRISMAUI_SHELL_SOURCES CONFIGURE_DEPENDS
    "${PRISMAUI_SHELL_APP_DIR}/index.html"
    "${PRISMAUI_SHELL_APP_DIR}/package*.json"
    "${PRISMAUI_SHELL_APP_DIR}/vite.config.ts"
    "${PRISMAUI_SHELL_APP_DIR}/tsconfig*.json"
    "${PRISMAUI_SHELL_APP_DIR}/src/*"
)

add_custom_command(
    OUTPUT "${PRISMAUI_SHELL_DIST_DIR}/index.html" "${PRISMAUI_BOOTSTRAP_JS}"
    COMMAND "${PRISMAUI_NPM_EXECUTABLE}" ci
    COMMAND "${PRISMAUI_NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${PRISMAUI_SHELL_APP_DIR}"
    DEPENDS ${PRISMAUI_SHELL_SOURCES}
    COMMENT "Building PrismaUI CEF shell"
    VERBATIM
)

add_custom_target(PrismaUIShell DEPENDS "${PRISMAUI_SHELL_DIST_DIR}/index.html")

# Embed the iframe bootstrap script (built by PrismaUIShell) into a generated
# C++ header that the renderer TU includes. Shared by both targets.
add_custom_command(
    OUTPUT "${PRISMAUI_BOOTSTRAP_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DBOOTSTRAP_JS=${PRISMAUI_BOOTSTRAP_JS}"
        "-DOUTPUT_HEADER=${PRISMAUI_BOOTSTRAP_HEADER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBootstrap.cmake"
    DEPENDS "${PRISMAUI_BOOTSTRAP_JS}" "${CMAKE_SOURCE_DIR}/cmake/EmbedBootstrap.cmake"
    COMMENT "Embedding PrismaUI iframe bootstrap script"
    VERBATIM
)
add_custom_target(PrismaUIBootstrapHeader DEPENDS "${PRISMAUI_BOOTSTRAP_HEADER}")

# Include compiler flags configuration
include(CompilerFlags)
include(ExternalDependencies)

# Collect source files
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

set(PRISMAUI_CEF_SHARED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefApp.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefApp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefRenderApp.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefRenderApp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/ProcessMessageNames.h"
)

set(PRISMAUI_CEF_SUBPROCESS_SOURCES
    ${PRISMAUI_CEF_SHARED_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Subprocess/SubprocessMain.cpp"
)

list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Subprocess/SubprocessMain.cpp")

# Create source groups for IDE
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "Source Files"
    FILES ${SOURCES}
)

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "CEF Subprocess"
    FILES ${PRISMAUI_CEF_SUBPROCESS_SOURCES}
)

# Add the plugin using CommonLibSSE function
add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "PrismaUI"
    AUTHOR "StarkMP <discord: starkmp>"
    EMAIL ""
    USE_ADDRESS_LIBRARY
    MINIMUM_SKSE_VERSION "2.0.1.04"
    VERSION ${PROJECT_VERSION}
    SOURCES ${SOURCES}
)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefApp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Shared/PrismaCefRenderApp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Browser/CefRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Cef/Browser/CefOsrClient.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

add_executable(PrismaUICefSubprocess WIN32
    ${PRISMAUI_CEF_SUBPROCESS_SOURCES}
)

target_include_directories(PrismaUICefSubprocess
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(PrismaUICefSubprocess
    PRIVATE
    libcef_dll_wrapper
    libcef_lib
    ${CEF_STANDARD_LIBS}
)

prismaui_configure_cef_executable(PrismaUICefSubprocess)
add_dependencies(${PROJECT_NAME} PrismaUICefSubprocess PrismaUIShell PrismaUIBootstrapHeader)
add_dependencies(PrismaUICefSubprocess PrismaUIBootstrapHeader)

# Include directories
target_include_directories(${PROJECT_NAME}
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CEF_ROOT}"
    "${CEF_ROOT}/include"
    "${CMAKE_BINARY_DIR}/generated"
)


# Link PrismaUI runtime libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    comctl32  # Required for SetWindowSubclass/RemoveWindowSubclass
    libcef_dll_wrapper
    libcef_lib
    ${CEF_STANDARD_LIBS}
    imm32     # Required for IME (Input Method Editor) support
)

# Delay-load CEF so it can be loaded from the packaged runtime directory
target_link_options(${PROJECT_NAME}
    PRIVATE
    "/DELAYLOAD:libcef.dll"
)

# Required for delay loading
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    delayimp
)
# Precompiled header
target_precompile_headers(${PROJECT_NAME}
    PRIVATE
    "src/PCH.h"
)
add_external_dependencies(${PROJECT_NAME})

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)


# Post-build: Copy CEF runtime files to output directory (for development)
prismaui_copy_cef_runtime(PrismaUICefSubprocess "$<TARGET_FILE_DIR:PrismaUICefSubprocess>")

# Post-build: Create distribution folder structure
set(DIST_ROOT "${CMAKE_SOURCE_DIR}/dist")
set(DIST_VERSION_DIR "${DIST_ROOT}/PrismaUI_${PROJECT_VERSION}$<$<CONFIG:Debug>:_Debug>")
set(DIST_PRISMAUI_DIR "${DIST_VERSION_DIR}/PrismaUI")
set(DIST_SKSE_PLUGINS_DIR "${DIST_VERSION_DIR}/SKSE/plugins")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${DIST_VERSION_DIR}"
    # Create distribution directories
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_PRISMAUI_DIR}/libs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_SKSE_PLUGINS_DIR}"
    
    # Copy assets folder contents to PrismaUI folder
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/" "${DIST_PRISMAUI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PRISMAUI_SHELL_DIST_DIR}/" "${DIST_PRISMAUI_DIR}/shell"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/NOTICES.txt" "${DIST_VERSION_DIR}/NOTICES.txt"
    
    # Copy PrismaUI DLL to SKSE/plugins
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${DIST_SKSE_PLUGINS_DIR}/"
    
    # Copy PrismaUI PDB to SKSE/plugins (if exists)
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_PDB_FILE:${PROJECT_NAME}>>,copy,true>
        "$<$<BOOL:$<TARGET_PDB_FILE:${PROJECT_NAME}>>:$<TARGET_PDB_FILE:${PROJECT_NAME}>>"
        "${DIST_SKSE_PLUGINS_DIR}/"

    # Copy CEF subprocess executable to PrismaUI/libs
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:PrismaUICefSubprocess>" "${DIST_PRISMAUI_DIR}/libs/"

    # Copy CEF subprocess PDB to PrismaUI/libs (if exists)
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_PDB_FILE:PrismaUICefSubprocess>>,copy,true>
        "$<$<BOOL:$<TARGET_PDB_FILE:PrismaUICefSubprocess>>:$<TARGET_PDB_FILE:PrismaUICefSubprocess>>"
        "${DIST_PRISMAUI_DIR}/libs/"
    
    COMMAND "${CMAKE_COMMAND}" -E echo "Creating distribution folder: ${DIST_VERSION_DIR}"
)

# Post-build: Copy CEF runtime files to distribution libs folder
prismaui_copy_cef_runtime(${PROJECT_NAME} "${DIST_PRISMAUI_DIR}/libs")

