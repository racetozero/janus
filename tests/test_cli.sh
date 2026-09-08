#!/bin/sh
set -eu

janus="${1:?path to janus is required}"
help="$("$janus")"
flag_help="$("$janus" --help)"

[ "$help" = "$flag_help" ]
printf '%s\n' "$help" | grep -Fq '_/ |\__,_|_| |_|\__,_|___/'
for command in sync import serve update; do
    printf '%s\n' "$help" | grep -Eq "^[[:space:]]+$command[[:space:]]"
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
