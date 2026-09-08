# Changelog

All notable changes to Janus are documented in this file.

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
