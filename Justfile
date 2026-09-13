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

lint: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake f -m release -y --toolchain=clang --cxflags="-Werror"
    VCPKG_ROOT="{{vcpkg_root}}" xmake
    VCPKG_ROOT="{{vcpkg_root}}" xmake project -k compile_commands
    clang-tidy --warnings-as-errors='clang-analyzer-*,cppcoreguidelines-owning-memory' -p=. src/*.cpp
    cppcheck --project=compile_commands.json --enable=warning,style,performance,portability --inconclusive --suppress=missingIncludeSystem --error-exitcode=1

asan: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake f -m debug -y --toolchain=clang --cxflags="-DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer" --ldflags="-fsanitize=address,undefined"
    VCPKG_ROOT="{{vcpkg_root}}" xmake
    ASAN_OPTIONS=detect_leaks=1 xmake test

tsan: setup
    VCPKG_ROOT="{{vcpkg_root}}" xmake f -m debug -y --toolchain=clang --cxflags="-DNDEBUG -fsanitize=thread -fno-omit-frame-pointer" --ldflags="-fsanitize=thread"
    VCPKG_ROOT="{{vcpkg_root}}" xmake
    xmake test

fuzz:
    mkdir -p build
    clang++ -std=c++23 -fsanitize=fuzzer,address -fno-omit-frame-pointer -Iinclude fuzz/fuzz_quote.cpp src/jsonl.cpp -o build/fuzz_quote
    ./build/fuzz_quote -runs=10000

fmt:
    clang-format -i src/*.cpp include/janus/*.hpp tests/*.cpp fuzz/*.cpp

clean:
    xmake clean
