set_xmakever("3.0.8")
set_languages("c++23")
set_defaultplat("windows")
set_defaultarchs("x64")
set_allowedplats("windows")
set_allowedarchs("x64")
set_defaultmode("releasedbg")
add_rules("mode.debug", "mode.releasedbg", "mode.release")
set_policy("build.c++.modules", true)
set_policy("package.requires_lock", true)

option("diagnostics")
    set_default(false)
    set_description("Compile detailed geometry and timing diagnostics; INI enables their output")
option_end()

option("commonlib")
    set_default(os.getenv("CommonLibSSE-NG") or "../CommonLibSSE-NG")
    set_description("CommonLibSSE-NG source checkout")
option_end()

includes(get_config("commonlib"))
set_project("GhostTrails")
set_version("1.0.7")
set_config("skyrim_se", true)
set_config("skyrim_ae", true)
set_config("skyrim_vr", true)

target("GhostTrails")
    add_rules("commonlibsse-ng.plugin", {
        name = "GhostTrails", author = "Newrite",
        description = "Pooled actor silhouettes with native and Papyrus APIs"
    })
    add_includedirs("src", "include")
    set_pcxxheader("src/PCH.h")
    add_files("src/*.cpp", "src/*.ixx")
    add_headerfiles("include/*.h", "src/PCH.h")
    add_cxxflags("/utf-8", "/Zc:preprocessor", {tools = "cl"})
    on_load(function (target)
        -- The CommonLib rule renders these configfiles after source discovery.
        -- Retain them when absent after `xmake clean`, so the first build also
        -- compiles the Windows resource and the generated SKSE export triad.
        target:add("files", path.join(target:autogendir(), "commonlib-plugin.rc"), {always_added = true})
        target:add("files", path.join(target:autogendir(), "commonlibsse-ng-plugin.cpp"), {always_added = true})
    end)
    if has_config("diagnostics") then
        add_defines("GHOSTTRAILS_DIAGNOSTICS")
        set_targetdir("build/diagnostics/$(plat)/$(arch)/$(mode)")
    end

target("GhostTrails.Tests")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src")
    add_files("src/Requests.ixx", "src/Timeline.ixx", "tests/RequestsTests.cpp")
    add_cxxflags("/utf-8", "/Zc:preprocessor", {tools = "cl"})

target("GhostTrails.SDKConsumer")
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("examples/NativeConsumer.ixx", "tests/SDKConsumer.cpp")
    add_cxxflags("/utf-8", "/Zc:preprocessor", {tools = "cl"})

target("GhostTrails.MaterialTests")
    set_kind("binary")
    set_default(false)
    add_files("src/Coverage.ixx", "tests/MaterialTests.cpp")
    add_cxxflags("/utf-8", "/Zc:preprocessor", {tools = "cl"})
