# Janus

`janus` moves resumable conversation threads between Claude Code and OpenAI
Codex. It is a small C++23 binary.

## Build

```sh
xmake f -m release
xmake
xmake test
```

XMake uses its built-in build cache and parallel compilation. vcpkg supplies
simdjson, xxHash, CLI11, and Catch2. Each dependency has one direct task: JSON
parsing, message fingerprints, CLI parsing, or tests. C++23 `std::print`
handles output.

The Justfile supplies the normal development commands:

```sh
just build
just test
just benchmark
just benchmark-suite
just fmt
```

It uses `VCPKG_ROOT` when that variable is set. Otherwise, it installs a
private vcpkg copy under `.tools/vcpkg`.

## Use

Run one sync:

```sh
xmake run janus sync
```

Import only one session. Janus detects its source harness from the standard
Codex rollout file name:

```sh
janus import SESSION.jsonl
```

Run it as a long-lived service:

```sh
janus serve --interval 2
```

The `serve` command stays in the foreground by default. Send `SIGTERM` or press
Ctrl-C for a clean stop. Use the built-in POSIX daemon mode when no service
manager is available:

```sh
janus serve --daemonize --interval 2
```

The state lock stops a second foreground or background process from using the
same state file. Windows does not support the built-in daemon option.

Custom data locations are supported:

```sh
janus sync \
  --claude-root ~/.claude/projects \
  --codex-root ~/.codex/sessions \
  --state ~/.local/state/janus/pairs.tsv
```

Janus finds unpaired JSONL sessions and creates a peer session in the
other tool. Later runs append new messages in both directions. The pair index
stops generated sessions from making an import loop.

Resume a generated session with the normal tool UI, or use its UUID:

```sh
claude --resume SESSION_UUID
codex resume SESSION_UUID
```

## Data and safety

- New peers use atomic file creation.
- Updates append complete JSONL records. Existing records are not rewritten.
- The state file contains paths only. It does not contain message text.
- Equal repeated messages are counted. This prevents common replay loops.
- JSONL input is streamed in 64 KiB blocks. Memory does not grow with session
  file size. One JSONL record can use at most 8 MiB; larger records are skipped.
- The service checks file sizes first. Idle checks do not parse session data.
- A file lock stops two Janus processes from using the same pair index.
- User and assistant text is copied. Tool calls and tool results become plain
  context text. Hidden reasoning, system instructions, token data, images, and
  active tool execution state are not copied.
- Stop both CLIs before the first large import. In service mode, a two-second
  interval reduces write overlap, but the source tools do not offer a shared
  file-lock protocol.

Back up your session folders before you use any third-party session converter.

## Format basis

The adapters follow the current JSONL readers in:

- Claude Code `src/utils/sessionStorage.ts`
- Codex `codex-rs/rollout/src/metadata.rs`, `recorder.rs`, and `list.rs`

These formats are internal and can change. Run `janus self-test` after an
upgrade. Keep a backup until you confirm that both tools can resume the copied
session.

## Architecture

Janus uses a streaming adapter pipeline:

1. The discovery layer finds Claude and Codex JSONL files.
2. The Claude or Codex adapter converts each record to a small message value.
3. A 128-bit fingerprint map removes messages that the peer already contains.
4. The target adapter appends native JSONL records and keeps native IDs,
   ordinals, and parent links valid.
5. The pair index stores paths and file sizes. The daemon only parses files
   that changed.

The process lock protects the pair index. New peers use a temporary file and
an atomic rename. Existing peers only receive complete appended records.

The source layout follows this pipeline:

- `jsonl` owns bounded JSONL input, output, time, and ID helpers.
- `adapters` reads and writes Claude and Codex formats. It uses simdjson and
  XXH3 128-bit fingerprints.
- `sync` owns discovery, pair state, locking, import, and daemon setup.
- `benchmark` owns the smoke test and synthetic performance tests.
- `main` defines the CLI and dispatches commands with CLI11.

Catch2 tests cover JSON escaping, conversion in both directions, and
idempotent copy behavior. Run them with `just test`.

## Releases

CI builds and tests Janus on Linux, macOS, and Windows. A pushed `vMAJOR.MINOR.PATCH`
tag creates a GitHub release with SHA-256 checksums and build attestations.
Release archives cover x86-64 and ARM64 on glibc Linux, static musl Linux,
macOS, and Windows.

Linux and macOS release binaries use benchmark-trained PGO and full LTO.
Windows release binaries use full LTO. Fat LTO objects are not used because
Janus ships a final executable, not reusable object files.

Normal builds get all dependencies from vcpkg. The isolated Alpine release
container compiles the same pinned CLI11, simdjson, and xxHash releases from
their upstream source because the vcpkg host tool requires glibc.
