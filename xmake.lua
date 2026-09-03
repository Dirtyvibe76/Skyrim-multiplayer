includes("External/CommonLibSSE-NG")

set_project("SkyrimMultiplayer")
set_version("0.0.1")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

add_requires("zlib")

target("SkyrimMultiplayer")
    add_rules("commonlibsse-ng.plugin", {
        name = "Skyrim Multiplayer",
        author = "Dirtyvibe76",
        description = "Server-authoritative Skyrim multiplayer client runtime adapter"
    })

    add_files("ClientPlugin/src/**.cpp")
    add_headerfiles(
        "ClientPlugin/src/**.h",
        "Shared/**.h"
    )

    add_includedirs(
        "ClientPlugin/src",
        "Shared"
    )

    set_pcxxheader("ClientPlugin/src/pch.h")

target("SkyrimMPServer")
    set_kind("binary")
    add_files("Server/src/**.cpp")
    add_packages("zlib")
    add_syslinks("bcrypt")
