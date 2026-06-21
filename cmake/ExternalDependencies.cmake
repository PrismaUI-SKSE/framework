# ExternalDependencies.cmake
# Configures external library subdirectories


include(commonlibsse)
include(cef)

# MinHook (vcpkg: minhook) provides the minhook::minhook target.
find_package(minhook CONFIG REQUIRED)

# Helper function to add external directories to a target
function(add_external_dependencies target_name)

    # Add target-specific include directories (PRIVATE to avoid pollution)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/external/commonlibsse-ng/include
    )

    # Link MinHook for trampoline-based detours
    target_link_libraries(${target_name} PRIVATE minhook::minhook)
endfunction()
