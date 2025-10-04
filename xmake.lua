-- set minimum xmake version
set_xmakever("2.8.2")

includes("lib/commonlibsse-ng")

set_project("PrismaUI")
set_version("1.1.0")
set_license("GPL-3.0")

set_languages("c++23")
set_warnings("allextra")

set_policy("package.requires_lock", true)

-- add_rules("mode.release")
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

local ULTRALIGHT_SDK_ROOT = "lib/ultralight-1.4.0"
local ULTRALIGHT_INCLUDE_DIR = ULTRALIGHT_SDK_ROOT .. "/include"
local ULTRALIGHT_BINARY_DIR = ULTRALIGHT_SDK_ROOT .. "/bin"
local ULTRALIGHT_LIBRARY_DIR = ULTRALIGHT_SDK_ROOT .. "/lib"

-- targets
target("PrismaUI")
    add_deps("commonlibsse-ng")

    add_rules("commonlibsse-ng.plugin", {
       name = "PrismaUI",
       author = "StarkMP <discord: starkmp>",
       description = "Skyrim Next-Gen Web UI Framework."
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")

    add_includedirs(ULTRALIGHT_INCLUDE_DIR)
    add_linkdirs(ULTRALIGHT_LIBRARY_DIR)
    
    add_links("UltralightCore", "AppCore", "Ultralight", "WebCore")
