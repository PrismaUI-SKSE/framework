# SKSEPlugin.cmake - Setup for SKSE plugin using CommonLibSSE-NG submodule

# CommonLibSSE-NG configuration
set(CommonLibPath "external/commonlibsse-ng")
set(CommonLibName "CommonLibSSE")
set(CommonLibBuildDir "${CMAKE_BINARY_DIR}/external_builds/${CommonLibName}")

# Extract version from CommonLibSSE's CMakeLists.txt (project() VERSION doesn't propagate to parent scope)
file(READ "${CMAKE_SOURCE_DIR}/${CommonLibPath}/CMakeLists.txt" _commonlib_cmakelists_content)
string(REGEX MATCH "VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${_commonlib_cmakelists_content}")
set(COMMONLIBSSE_VERSION "${CMAKE_MATCH_1}" CACHE STRING "CommonLibSSE-NG version" FORCE)
message(STATUS "Configuring CommonLibSSE-NG version ${COMMONLIBSSE_VERSION}")

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# Disable CommonLibSSE tests when building as subdirectory
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)

# Check if CommonLibSSE has already been built
set(COMMONLIB_STAMP_FILE "${CommonLibBuildDir}/${CommonLibName}.stamp")
# Add CommonLibSSE-NG as a subdirectory with EXCLUDE_FROM_ALL
add_subdirectory("${CommonLibPath}" "${CommonLibBuildDir}" EXCLUDE_FROM_ALL)

# Create stamp file after configuration (informational only)
if(NOT EXISTS "${COMMONLIB_STAMP_FILE}")
    file(WRITE "${COMMONLIB_STAMP_FILE}" "Configured on ${CMAKE_SYSTEM_NAME}")
endif()

# Include the CommonLibSSE helper cmake functions (provides add_commonlibsse_plugin macro)
include("${CommonLibPath}/cmake/CommonLibSSE.cmake")

# Expose CommonLibSSE version to C++ code
add_compile_definitions(COMMONLIBSSE_VERSION="${COMMONLIBSSE_VERSION}")
