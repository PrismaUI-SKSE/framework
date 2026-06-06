# CompilerFlags.cmake
# Configures compiler-specific flags and optimizations

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Statically link the C runtime (avoids MSVC redistributable DLLs)
if(MSVC)
    string(REGEX REPLACE "/EH[scra-]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(APPEND CMAKE_CXX_FLAGS " /permissive- /Zc:preprocessor /MP /W4 /utf-8")

    # Add compilation speed optimizations
    # /d2ReducedOptimizeHugeFunctions - Faster optimization for large functions
    string(APPEND CMAKE_CXX_FLAGS " /d2ReducedOptimizeHugeFunctions")
    
    # Code generation optimizations
    string(APPEND CMAKE_CXX_FLAGS " /Gy")    # Function-level linking (enables dead code elimination)
    string(APPEND CMAKE_CXX_FLAGS " /Gw")    # Optimize global data (similar to /Gy for globals)
    #string(APPEND CMAKE_CXX_FLAGS " /arch:AVX")  # Use AVX (since 2011) SIMD instructions
    #string(APPEND CMAKE_CXX_FLAGS " /arch:AVX2")  # Use AVX2 (since 2013) SIMD instructions
    
    # Strict C++ standard conformance flags
    string(APPEND CMAKE_CXX_FLAGS " /Zc:inline")
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:alignedNew") # Enforce aligned new/delete
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:auto") # Enforce 'auto' type deduction
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:__cplusplus") # Correct __cplusplus macro value
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:externC") # Enforce extern "C" linkage rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:externConstexpr") # Enforce extern constexpr rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:forScope") # Enforce for-loop scope rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:hiddenFriend") # Enforce hidden friend functions
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:implicitNoexcept") # Enforce implicit noexcept
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:lambda") # Enforce lambda rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:noexceptTypes") # Enforce noexcept type rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:preprocessor") # Enforce preprocessor rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:referenceBinding") # Enforce reference binding rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:rvalueCast") # Enforce rvalue cast rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:sizedDealloc") # Enforce sized deallocation
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:strictStrings") # Enforce strict string rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:ternary") # Enforce ternary operator rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:threadSafeInit") # Enforce thread-safe initialization
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:trigraphs") # Enforce trigraph rules
    #string(APPEND CMAKE_CXX_FLAGS " /Zc:wchar_t") # Enforce wchar_t rules
    #string(APPEND CMAKE_CXX_FLAGS_RELEASE " /GL") # Whole program optimization 
    
    # Apply critical flags to all build configurations (Debug/Release, C/C++)
    # This ensures consistency and overrides CMake defaults that may vary by configuration
    foreach(flag_var
        CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG)
        # Remove any existing exception handling flags and apply /EHa (asynchronous)
        # Required for _set_se_translator to catch Windows SEH exceptions
        string(REGEX REPLACE "/EH[scra-z]+" "" ${flag_var} "${${flag_var}}")
        string(APPEND ${flag_var} " /EHa /bigobj")
        # Switch from dynamic (/MD) to static (/MT) runtime linking
        # This eliminates dependency on MSVC redistributables for end users
        string(REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
    endforeach()
    
    # Add flags for PDB generation in Release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    #set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF /LTCG") # Enable if using /GL
    #set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF /LTCG")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF") # /GL and /LTCG disabled to see if helps with debugging
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    

endif()
