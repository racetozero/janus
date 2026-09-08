set_project("janus")
set_version("0.0.2")
set_languages("c++23")
set_policy("build.ccache", true)

add_rules("mode.debug", "mode.release")
add_requires("vcpkg::catch2", "vcpkg::cli11", "vcpkg::simdjson", "vcpkg::sqlite3", "vcpkg::xxhash")

target("janus")
    set_kind("binary")
    set_targetdir("dist")
    add_includedirs("include")
    add_files("src/main.cpp", "src/jsonl.cpp", "src/adapters.cpp", "src/sync.cpp", "src/update.cpp")
    add_packages("vcpkg::cli11", "vcpkg::simdjson", "vcpkg::sqlite3", "vcpkg::xxhash")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
    add_defines('JANUS_VERSION="' .. (os.getenv("JANUS_VERSION") or "0.0.2") .. '"')
    if is_mode("release") then
        set_policy("build.optimization.lto", true)
    end
    set_warnings("all", "extra")

target("janus-benchmark")
    set_kind("binary")
    set_default(false)
    set_targetdir("dist")
    add_includedirs("include")
    add_files("src/benchmark_main.cpp", "src/benchmark.cpp", "src/jsonl.cpp", "src/adapters.cpp")
    add_packages("vcpkg::simdjson", "vcpkg::sqlite3", "vcpkg::xxhash")
    set_warnings("all", "extra")

target("janus-tests")
    set_kind("binary")
    add_includedirs("include")
    add_files("tests/*.cpp", "src/jsonl.cpp", "src/adapters.cpp", "src/sync.cpp")
    add_packages("vcpkg::catch2", "vcpkg::simdjson", "vcpkg::sqlite3", "vcpkg::xxhash")
    set_warnings("all", "extra")
    add_tests("unit")
