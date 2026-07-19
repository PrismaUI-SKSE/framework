# Define variables

set(ARCHIVE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/ultralight-free-sdk-1.4.0-win-x64.7z")

# Check if archive exists
if(NOT EXISTS "${ARCHIVE_FILE}")
    message(FATAL_ERROR  
    " Ultralight SDK archive not found in 'external' folder.\n"
    " Download version 1.4.1-dev from https://ultralig.ht/download"
    )
endif()

set(DESTINATION_DIR "${BUILD_ROOT}/external_builds/ultralight")

# Create the destination directory if it doesn't exist
file(MAKE_DIRECTORY "${DESTINATION_DIR}")

# Extract the archive
file(ARCHIVE_EXTRACT
    INPUT "${ARCHIVE_FILE}"
    DESTINATION "${DESTINATION_DIR}"
)

# Ultralight SDK paths
# check if extraction was successful and exists
if(EXISTS "${DESTINATION_DIR}/include" AND EXISTS "${DESTINATION_DIR}/bin" AND EXISTS "${DESTINATION_DIR}/lib" AND EXISTS "${DESTINATION_DIR}/resources" AND EXISTS "${DESTINATION_DIR}/inspector")
    set(ULTRALIGHT_SDK_ROOT "${DESTINATION_DIR}")
    set(ULTRALIGHT_INCLUDE_DIR "${ULTRALIGHT_SDK_ROOT}/include")
    set(ULTRALIGHT_BINARY_DIR "${ULTRALIGHT_SDK_ROOT}/bin")
    set(ULTRALIGHT_LIBRARY_DIR "${ULTRALIGHT_SDK_ROOT}/lib")
    set(ULTRALIGHT_RESOURCES_DIR "${ULTRALIGHT_SDK_ROOT}/resources")
    set(ULTRALIGHT_INSPECTOR_DIR "${ULTRALIGHT_SDK_ROOT}/inspector")
else()
    message(FATAL_ERROR "Ultralight SDK extraction failed or the expected files are missing.")
endif()


