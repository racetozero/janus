#!/usr/bin/env bash
set -euo pipefail

asset="${1:?asset name is required}"
version="${2:?version is required}"
export JANUS_VERSION="$version"
if [[ -z "${VCPKG_ROOT:-}" && -x .tools/vcpkg/vcpkg ]]; then
  export VCPKG_ROOT="$PWD/.tools/vcpkg"
fi
profile_dir="$(mktemp -d)"
cleanup() {
  xmake f -c -m release -y --cxflags= --ldflags= >/dev/null 2>&1 || true
  rm -rf "$profile_dir"
}
trap cleanup EXIT

configure() {
  xmake f -c -m release -y --cxflags="$1" --ldflags="$1"
  xmake build janus janus-benchmark janus-tests
}

if [[ "$(uname -s)" == "Darwin" ]]; then
  export LLVM_PROFILE_FILE="$profile_dir/janus-%p.profraw"
  configure "-fprofile-instr-generate"
  ./dist/janus-benchmark 50000 >/dev/null
  xcrun llvm-profdata merge -output="$profile_dir/janus.profdata" "$profile_dir"/*.profraw
  configure "-fprofile-instr-use=$profile_dir/janus.profdata"
else
  configure "-fprofile-generate=$profile_dir"
  ./dist/janus-benchmark 50000 >/dev/null
  configure "-fprofile-use=$profile_dir -fprofile-correction -Wno-missing-profile"
fi

./dist/janus-tests
strip dist/janus
tar -czf "dist/$asset.tar.gz" -C dist janus -C .. README.md LICENSE
if command -v sha256sum >/dev/null 2>&1; then
  (cd dist && sha256sum "$asset.tar.gz" >"$asset.tar.gz.sha256")
else
  (cd dist && shasum -a 256 "$asset.tar.gz" >"$asset.tar.gz.sha256")
fi
