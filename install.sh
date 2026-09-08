#!/bin/sh
set -eu

REPOSITORY="${JANUS_REPOSITORY:-racetozero/janus}"
RELEASES_URL="${JANUS_RELEASES_URL:-https://github.com/$REPOSITORY/releases}"

say() {
    printf '%s\n' "$*" >&2
}

fail() {
    say "janus installer: $*"
    exit 1
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

normalize_tag() {
    requested="$1"
    case "$requested" in
        v*) tag="$requested" ;;
        *) tag="v$requested" ;;
    esac
    version="${tag#v}"
    case "$version" in
        '' | *[!0-9A-Za-z.+-]*) fail "invalid version: $requested" ;;
    esac
}

resolve_tag() {
    requested="${JANUS_VERSION:-}"
    if [ -n "$requested" ] && [ "$requested" != "latest" ]; then
        normalize_tag "$requested"
        return
    fi

    [ -z "${JANUS_RELEASES_URL:-}" ] ||
        fail "set JANUS_VERSION when JANUS_RELEASES_URL is set"
    command_exists curl || fail "curl is required to find the latest release"
    final_url="$(curl --proto '=https' --tlsv1.2 -fsSL -o /dev/null -w '%{url_effective}' "$RELEASES_URL/latest")" ||
        fail "could not find the latest release"
    resolved="${final_url%/}"
    resolved="${resolved##*/}"
    [ -n "$resolved" ] || fail "the latest release URL has no tag"
    normalize_tag "$resolved"
}

download_release() {
    asset_base="$RELEASES_URL/download/$tag"
    download_file "$asset_base/$archive_name" "$temporary_directory/$archive_name" ||
        fail "could not download $archive_name"
    download_file "$asset_base/$checksum_name" "$temporary_directory/$checksum_name" ||
        fail "could not download $checksum_name"
}

verify_staged_binary() {
    command_exists tar || fail "tar is required to inspect the release"
    probe_directory="$temporary_directory/probe-$target"
    mkdir "$probe_directory"
    tar -xzf "$temporary_directory/$archive_name" -C "$probe_directory"
    probe_output=''
    if probe_output="$("$probe_directory/janus" --version 2>&1)"; then
        return 0
    fi
    case "$probe_output" in
        *GLIBC_[0-9.]*not\ found* | *version*GLIBC_[0-9.]*not\ found*) return 2 ;;
        *) return 1 ;;
    esac
}

detect_target() {
    if [ -n "${JANUS_TARGET:-}" ]; then
        target="$JANUS_TARGET"
    else
        os="$(uname -s)" || fail "could not detect the operating system"
        machine="$(uname -m)" || fail "could not detect the processor"
        case "$machine" in
            arm64 | aarch64) architecture="aarch64" ;;
            x86_64 | amd64) architecture="x86_64" ;;
            *) fail "unsupported processor: $machine" ;;
        esac

        case "$os" in
            Darwin) target="$architecture-apple-darwin" ;;
            Linux)
                libc="${JANUS_LIBC:-}"
                if [ -z "$libc" ]; then
                    if command_exists getconf && getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
                        libc="gnu"
                    elif command_exists ldd && ldd --version 2>&1 | grep -qi musl; then
                        libc="musl"
                    else
                        libc="gnu"
                    fi
                fi
                case "$libc" in
                    gnu | musl) ;;
                    *) fail "JANUS_LIBC must be gnu or musl" ;;
                esac
                target="$architecture-unknown-linux-$libc"
                ;;
            *) fail "unsupported operating system: $os" ;;
        esac
    fi

    case "$target" in
        aarch64-apple-darwin | x86_64-apple-darwin | aarch64-unknown-linux-gnu | aarch64-unknown-linux-musl | x86_64-unknown-linux-gnu | x86_64-unknown-linux-musl) ;;
        *) fail "unsupported target: $target" ;;
    esac
}

download_file() {
    source_url="$1"
    destination="$2"
    case "$source_url" in
        file://*) cp "${source_url#file://}" "$destination" ;;
        https://*)
            if command_exists curl; then
                curl --proto '=https' --tlsv1.2 -fsSL "$source_url" -o "$destination"
            elif command_exists wget; then
                wget --https-only --secure-protocol=TLSv1_2 -q "$source_url" -O "$destination"
            else
                fail "curl or wget is required to download release files"
            fi
            ;;
        *) fail "release downloads must use https or file URLs" ;;
    esac
}

verify_archive() {
    archive_file="$temporary_directory/$archive_name"
    expected="$(awk 'NF { print $1; exit }' "$temporary_directory/$checksum_name")"
    printf '%s\n' "$expected" | grep -Eq '^[0-9A-Fa-f]{64}$' ||
        fail "the checksum file is invalid"

    if command_exists sha256sum; then
        actual="$(sha256sum "$archive_file" | awk '{ print $1 }')"
    elif command_exists shasum; then
        actual="$(shasum -a 256 "$archive_file" | awk '{ print $1 }')"
    else
        fail "sha256sum or shasum is required to verify the release"
    fi
    [ "$(printf '%s' "$actual" | tr 'A-F' 'a-f')" = "$(printf '%s' "$expected" | tr 'A-F' 'a-f')" ] ||
        fail "checksum verification failed for $archive_name"
}

install_binary() {
    extract_directory="$temporary_directory/extract"
    mkdir "$extract_directory"
    tar -xzf "$temporary_directory/$archive_name" -C "$extract_directory"
    binary="$extract_directory/janus"
    [ -f "$binary" ] || fail "the release archive does not contain janus"

    install_directory="${JANUS_INSTALL_DIR:-${HOME:?HOME is not set}/.local/bin}"
    mkdir -p "$install_directory"
    staged="$(mktemp "$install_directory/.janus.install.XXXXXX")" ||
        fail "could not create a staged executable in $install_directory"
    cp "$binary" "$staged"
    chmod 0755 "$staged"
    mv -f "$staged" "$install_directory/janus"
    staged=''

    say "Installed janus $version to $install_directory/janus"
    case ":${PATH:-}:" in
        *":$install_directory:"*) ;;
        *) say "Add $install_directory to PATH to run janus from any directory." ;;
    esac
}

umask 077
staged=''
temporary_directory="$(mktemp -d "${TMPDIR:-/tmp}/janus-install.XXXXXX")" ||
    fail "could not create a temporary directory"
cleanup() {
    rm -rf "$temporary_directory"
    [ -z "$staged" ] || rm -f "$staged"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

resolve_tag
detect_target
archive_name="janus-$target.tar.gz"
checksum_name="$archive_name.sha256"

say "Installing janus $version for $target"
command_exists tar || fail "tar is required to extract $archive_name"
download_release
verify_archive
if case "$target" in *-unknown-linux-gnu) true ;; *) false ;; esac; then
    if verify_staged_binary; then
        :
    else
        probe_status=$?
        if [ "$probe_status" -eq 2 ]; then
            target="${target%-gnu}-musl"
            archive_name="janus-$target.tar.gz"
            checksum_name="$archive_name.sha256"
            say "The glibc runtime is too old; using the musl release for $target"
            download_release
            verify_archive
        else
            fail "the release binary could not run on this system"
        fi
    fi
fi
install_binary
