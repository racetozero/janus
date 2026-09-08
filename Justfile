vcpkg_root := env_var_or_default("VCPKG_ROOT", justfile_directory() + "/.tools/vcpkg")

default: build

setup:
    if [ ! -x "{{vcpkg_root}}/vcpkg" ]; then mkdir -p "{{vcpkg_root}}"; git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "{{vcpkg_root}}"; "{{vcpkg_root}}/bootstrap-vcpkg.sh" -disableMetrics; fi

build: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake f -m release -y --cxflags= --ldflags=
    VCPKG_ROOT="{{vcpkg_root}}" xmake

test: build
    xmake test
    tests/test_cli.sh dist/janus

benchmark: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake build janus-benchmark
    xmake run janus-benchmark 10000

benchmark-suite: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake build janus-benchmark
    xmake run janus-benchmark --suite

fmt:
    clang-format -i src/*.cpp include/janus/*.hpp tests/*.cpp

clean:
    xmake clean
