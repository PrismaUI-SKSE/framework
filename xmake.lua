-- set minimum xmake version
set_xmakever("2.8.2")

includes("lib/commonlibsse-ng")

set_project("PrismaUI-Example-Plugin")
set_version("0.2.0-beta")
set_license("GPL-3.0")

set_languages("c++23")
set_warnings("allextra")

set_policy("package.requires_lock", true)

add_rules("mode.release")
add_rules("plugin.vsxmake.autoupdate")

-- targets
target("PrismaUI-Example-Plugin")
    add_deps("commonlibsse-ng")

    add_rules("commonlibsse-ng.plugin", {
       name = "PrismaUI-Example-Plugin",
       author = "StarkMP <discord: starmp>",
       description = "SKSE64 plugin template using CommonLibSSE-NG and PrismaUI"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
