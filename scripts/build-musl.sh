#!/usr/bin/env bash
set -euo pipefail

asset="${1:?asset name is required}"
version="${2:?version is required}"
profile_dir="$(mktemp -d)"
trap 'rm -rf "$profile_dir"' EXIT
sources=(src/*.cpp)
common=(-std=c++23 -O3 -DNDEBUG -flto -static -pthread -Iinclude
        -I/tmp/CLI11/include -I/tmp/simdjson/include -I/tmp/xxhash -DCLI11_COMPILE
        -DJANUS_VERSION="\"$version\"" "${sources[@]}" /tmp/CLI11/src/Precompile.cpp
        /tmp/simdjson/src/simdjson.cpp /tmp/xxhash/xxhash.c)

g++ "${common[@]}" -fprofile-generate="$profile_dir" -o dist/janus
./dist/janus benchmark 50000 >/dev/null
g++ "${common[@]}" -fprofile-use="$profile_dir" -fprofile-correction -Wno-missing-profile \
  -o dist/janus
./dist/janus self-test
strip dist/janus
tar -czf "dist/$asset.tar.gz" -C dist janus -C .. README.md LICENSE
(cd dist && sha256sum "$asset.tar.gz" >"$asset.tar.gz.sha256")
