# Changelog

All notable changes to Janus are documented in this file.

## [Unreleased]

### Changed

- A deleted session stays deleted after a restart. Run `janus import` to create it again.
- Janus does not read or write archived, hidden, or ended sessions, and catches up after a restore.
- `janus serve` uses a new harness store without a restart.
- `janus serve --daemonize` writes messages and errors to the state file path plus `.log`.
- `janus serve` increases its delay after a failed sync, up to 5 minutes.
- `janus update` makes no change when the running version is newer than the latest release.
- `install.sh` does not probe a target that you set with `JANUS_TARGET`.

### Fixed

- Stop duplicate sessions after a failed peer creation, a failed copy, or a failed state save.
- Stop a bad state file row from causing duplicate sessions in `janus serve`.
- A failure in one session or store no longer stops the others.
- A deleted or archived session no longer stops its group.
- Do not lose a message that arrives during a sync.
- Do not join a new record to an unterminated line in a session file.
- Read a complete final record that has no newline.
- Treat a short read as an error, not as the end of a session.
- Use the last record in the parent chain as the parent of each new record.
- Write OpenCode messages with strictly increasing times and IDs, so they keep their source order.
- An unreadable new session no longer stops discovery of other sessions in its store.
- An idle sync no longer parses unchanged JSONL files.
- Replace the state file in one step on Windows, so a crash cannot lose it.
- Report the location of the previous executable if a Windows update cannot restore it.
- Remove the staged file if the Windows installer cannot complete its final move.

## [0.0.3] - 2026-09-13

### Added

- Add compiler warnings, clang-tidy, Cppcheck, CodeQL, fuzzing, and sanitizer checks.
- Add resource limits for JSONL input and synchronization groups.

### Fixed

- Keep synchronization groups valid after a reload.
- Publish GitHub releases only after all release assets are available.

## [0.0.2] - 2026-09-08

### Changed

- Release the multi-harness synchronization and GitHub update command.

## [0.0.1] - 2026-09-07

### Added

- Bidirectional session transfer between Claude Code and OpenAI Codex.
- Session adapters for KISS, Pi, OpenClaw, Hermes, and OpenCode.
- In-place updates from GitHub Releases with `janus update`.
- Continuous synchronization with an optional background daemon mode.
- C++23 builds with XMake, vcpkg, and build caching.
- Catch2 tests and maintainer-only synthetic benchmarks.
- CI for Linux, macOS, and Windows.
- Tag releases for x86-64 and ARM64, including glibc and static musl Linux archives.
- Release optimization with profile-guided optimization and full link-time optimization where supported.

### Changed

- Use portable standard streams on C++23 toolchains that do not provide `<print>`.

[0.0.1]: https://github.com/racetozero/janus/releases/tag/v0.0.1
[0.0.2]: https://github.com/racetozero/janus/releases/tag/v0.0.2
[0.0.3]: https://github.com/racetozero/janus/releases/tag/v0.0.3
