#!/bin/sh
set -eu

janus="${1:?path to janus is required}"
help="$("$janus")"
flag_help="$("$janus" --help)"

[ "$help" = "$flag_help" ]
printf '%s\n' "$help" | grep -Fq '_/ |\__,_|_| |_|\__,_|___/'
for command in sync import serve update; do
    printf '%s\n' "$help" | grep -Eq "^[[:space:]]+${command}[[:space:]]"
done
for command in benchmark benchmark-suite self-test; do
    if printf '%s\n' "$help" | grep -Fq "$command"; then
        printf 'public help contains maintainer command: %s\n' "$command" >&2
        exit 1
    fi
    if "$janus" "$command" >/dev/null 2>&1; then
        printf 'public CLI accepts maintainer command: %s\n' "$command" >&2
        exit 1
    fi
done

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) exit 0 ;;
esac

work="$(mktemp -d)"
state="$work/state/pairs.tsv"
stop_daemon() {
    pkill -TERM -f "$state" 2>/dev/null || :
}
cleanup() {
    stop_daemon
    rm -rf "$work"
}
trap cleanup EXIT

# A background serve must keep a readable failure log and slow its retries.
mkdir -p "$work/state"
printf 'not a valid state row\n' >"$state"
HOME="$work" "$janus" serve --daemonize --interval 1 --state "$state"
sleep 4
log="$state.log"
[ -f "$log" ] || {
    printf 'background serve did not create %s\n' "$log" >&2
    exit 1
}
grep -Fq 'line 1' "$log"
failures="$(grep -Fc 'sync error' "$log")"
[ "$failures" -ge 1 ] && [ "$failures" -le 3 ] || {
    printf 'background serve retried %s times in 4 seconds; expected 1 to 3\n' "$failures" >&2
    exit 1
}
stop_daemon
attempt=0
while pgrep -f "$state" >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    [ "$attempt" -le 50 ] || {
        printf 'background serve did not stop after SIGTERM\n' >&2
        exit 1
    }
    sleep 0.1
done

# An explicit target is not probed, because it can be for another processor.
target=x86_64-unknown-linux-gnu
archive="janus-$target.tar.gz"
release="$work/release/download/v0.0.0"
mkdir -p "$release" "$work/payload"
printf '#!/bin/sh\nexit 1\n' >"$work/payload/janus"
chmod 0755 "$work/payload/janus"
tar -czf "$release/$archive" -C "$work/payload" janus
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$release/$archive" >"$release/$archive.sha256"
else
    shasum -a 256 "$release/$archive" >"$release/$archive.sha256"
fi
JANUS_VERSION=0.0.0 JANUS_RELEASES_URL="file://$work/release" JANUS_TARGET="$target" \
    JANUS_INSTALL_DIR="$work/bin" sh "$(dirname "$0")/../install.sh" 2>/dev/null
[ -x "$work/bin/janus" ]
