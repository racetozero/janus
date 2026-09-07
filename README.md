# Janus

Move live coding sessions between Claude Code and OpenAI Codex.

Janus converts each harness's native JSONL records and keeps paired sessions in
sync. Use it for one import, one bidirectional sync, or a small background
service. The result stays resumable in both tools.

## Why Janus

- **Keep context:** continue the same task in Claude Code or Codex.
- **Work both ways:** copy new user and assistant messages in each direction.
- **Stay native:** produce normal session files for each harness.
- **Use little memory:** stream JSONL in bounded blocks instead of loading a
  full session.
- **Run anywhere:** use one binary on Linux, macOS, or Windows.
- **Automate it:** run once, serve in the foreground, or daemonize on POSIX.

## Install

macOS and Linux:

```bash
curl -LsSf https://raw.githubusercontent.com/racetozero/janus/main/install.sh | sh
```

Windows:

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://raw.githubusercontent.com/racetozero/janus/main/install.ps1 | iex"
```

The installer detects the operating system, processor, and Linux C library. It
downloads the correct release, verifies its SHA-256 checksum, and installs
`janus` in the user binary directory.

## Quick start

Run one bidirectional sync:

```bash
janus sync
```

Import one Claude Code or Codex session:

```bash
janus import SESSION.jsonl
```

Keep all sessions in sync:

```bash
janus serve --interval 2
```

Run Janus as its own background daemon on macOS or Linux:

```bash
janus serve --daemonize --interval 2
```

Windows supports foreground service mode but not `--daemonize`. Use the normal
tool UI to resume a generated session, or use its UUID:

```bash
claude --resume SESSION_UUID
codex resume SESSION_UUID
```

## Choose data locations

Janus uses the standard Claude Code and Codex session folders by default. You
can set every path:

```bash
janus sync \
  --claude-root ~/.claude/projects \
  --codex-root ~/.codex/sessions \
  --state ~/.local/state/janus/pairs.tsv
```

The pair index prevents an import loop. A process lock prevents two Janus
processes from changing the same index.

## Performance

Janus first checks file sizes. An idle service check does not parse unchanged
sessions. Changed JSONL files are read in 64 KiB blocks, and memory use does not
grow with the full file size.

The latest local release-mode benchmark ran on Apple ARM64. It used synthetic
messages with a 240-byte payload and no network calls:

| Messages | Input | Claude to Codex | Throughput | Codex to Claude |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.60 MiB | 9.37 ms | 64.56 MiB/s | 8.68 ms |
| 10,000 | 6.06 MiB | 65.93 ms | 91.90 MiB/s | 78.13 ms |
| 50,000 | 30.34 MiB | 284.98 ms | 106.46 MiB/s | 375.74 ms |

Results depend on storage, processor, message shape, and source format. Run the
same suite on your system with `just benchmark-suite`.

Release binaries use full link-time optimization. Linux and macOS builds also
use benchmark-trained profile-guided optimization. Windows builds use full LTO.

## Supported releases

Each release supplies SHA-256 checksums and build attestations for these
targets:

| System | Processor | Runtime | Release target |
| --- | --- | --- | --- |
| Linux | x86-64 | glibc | `x86_64-unknown-linux-gnu` |
| Linux | ARM64 | glibc | `aarch64-unknown-linux-gnu` |
| Linux | x86-64 | static musl | `x86_64-unknown-linux-musl` |
| Linux | ARM64 | static musl | `aarch64-unknown-linux-musl` |
| macOS | Intel | native | `x86_64-apple-darwin` |
| macOS | Apple silicon | native | `aarch64-apple-darwin` |
| Windows | x86-64 | MSVC | `x86_64-pc-windows-msvc` |
| Windows | ARM64 | MSVC | `aarch64-pc-windows-msvc` |

The musl files are static binaries. The glibc files use the normal GNU Linux
runtime. macOS files support Intel and Apple silicon.

## Data and safety

- New peers use atomic file creation.
- Updates append complete JSONL records. Janus does not rewrite old records.
- The state file stores paths and file sizes, not message text.
- Equal repeated messages remain distinct while replay loops are removed.
- One JSONL record can use at most 8 MiB. Janus skips larger records.
- Tool calls and results become plain context text.
- Hidden reasoning, system instructions, token data, images, and live tool
  state are not copied.

Stop both harnesses before the first large import. Back up session folders
before you use any third-party session converter. Claude Code and Codex do not
publish a shared lock or a stable cross-harness session format.

## How it works

1. Discovery finds Claude Code and Codex JSONL files.
2. A harness adapter converts each record to a small message value.
3. XXH3 128-bit fingerprints find messages that the peer already has.
4. The target adapter appends native records with valid IDs and parent links.
5. The pair index stores paths and file sizes for the next sync.

The code follows the same pipeline:

- `jsonl` owns bounded input, output, time, and ID helpers.
- `adapters` reads and writes both formats with simdjson.
- `sync` owns discovery, state, locking, import, and daemon setup.
- `benchmark` owns the smoke test and synthetic benchmarks.
- `main` defines the CLI with CLI11.

The adapters follow the current readers in Claude Code
`src/utils/sessionStorage.ts` and Codex `codex-rs/rollout`. These internal
formats can change. Run `janus self-test` after a harness upgrade.

## Develop

Janus uses C++23, XMake, vcpkg, Catch2, and Google-style clang-format. XMake
uses its build cache and parallel compilation. The Justfile manages a private
vcpkg copy when `VCPKG_ROOT` is not set.

```bash
just build
just test
just benchmark
just benchmark-suite
just fmt
```

Dependencies have one direct purpose: CLI11 parses commands, simdjson parses
JSON, xxHash creates fingerprints, and Catch2 runs tests.

## Release

A `vMAJOR.MINOR.PATCH` tag starts the multi-system release workflow. It creates
archives, checksums, attestations, and a GitHub release. Normal builds use
vcpkg. The Alpine musl build compiles the same pinned dependency versions from
source because the vcpkg host tool requires glibc.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

## License

MIT. See [LICENSE](LICENSE).
