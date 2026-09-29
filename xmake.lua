-- Perfected Wheeler for The Elder Scrolls IV: Oblivion Remastered (OBSE64 plugin).
-- The game's own Quick Keys radial is the wheel; this plugin manages what stands behind its eight slots
-- (two wheels, several entries a slot, L3/R3 editing, favourites). No menu and no drawing of its own
-- (the owner, 2026-09-26).
-- rule 45: no build-machine paths in any compiled object - set BEFORE includes() so CommonLibOB64's own library
-- target gets it too (a std::source_location in an OBSE header reached through the PCH's absolute -FI path).
-- /d1trimfile strips the project folder from __FILE__ and std::source_location. The flag is wrapped in a TABLE so
-- xmake passes it as one quoted argument: a bare string is split on the path's spaces, and /d1trimfile:"<dir>"
-- reaches cl with the quote characters in the prefix, which then matches nothing (measured 2026-09-29). No trailing
-- separator. /PDBALTPATH:%_PDB% makes the debug directory record only the PDB's file name (it ships beside the DLL).
add_cxflags({"/d1trimfile:$(projectdir)"}, {force = true, expand = false})
add_shflags("/PDBALTPATH:%_PDB%", {force = true})

includes("lib/commonlibob64")

set_project("PerfectedWheeler")
set_version("0.0.0")
set_license("GPL-3.0-or-later")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("minhook")

target("PerfectedWheeler")
    add_rules("commonlibob64.plugin", {
        name = "PerfectedWheeler",
        author = "ApocryphaRealm",
        description = "Perfected Wheeler - the Quick Keys radial, augmented (Oblivion Remastered)"
    })
    add_packages("minhook")
    add_syslinks("user32")
    on_load(function (target)
        target:add("defines", "PW_VERSION=\"" .. (target:version() or "0.0.0") .. "\"")
    end)
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
