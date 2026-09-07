#include <CLI/CLI.hpp>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

#include "janus/benchmark.hpp"
#include "janus/sync.hpp"

#ifndef JANUS_VERSION
#define JANUS_VERSION "0.0.1"
#endif

namespace {

volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

void add_paths(CLI::App& command, janus::Options& options) {
  command.add_option("--claude-root", options.claude_root, "Claude session root");
  command.add_option("--codex-root", options.codex_root, "Codex session root");
  command.add_option("--state", options.state, "Pair state file");
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Move sessions between Claude Code and OpenAI Codex", "janus"};
  try {
    app.set_version_flag("--version", "janus " JANUS_VERSION);
    app.require_subcommand(1);

    janus::Options sync_options;
    CLI::App& sync = *app.add_subcommand("sync", "Run one bidirectional sync");
    add_paths(sync, sync_options);

    janus::Options import_options;
    janus::fs::path import_path;
    CLI::App& import = *app.add_subcommand("import", "Import one session");
    import.add_option("session", import_path, "Session JSONL file")->required();
    add_paths(import, import_options);

    janus::Options serve_options;
    CLI::App& serve = *app.add_subcommand("serve", "Keep sessions in sync");
    add_paths(serve, serve_options);
    serve.add_option("--interval", serve_options.interval, "Sync interval in seconds")
        ->check(CLI::PositiveNumber);
    serve.add_flag("--daemonize", serve_options.daemonize, "Run in the background");

    std::size_t message_count = 10000;
    CLI::App& benchmark = *app.add_subcommand("benchmark", "Run one synthetic benchmark");
    benchmark.add_option("messages", message_count, "Synthetic message count")
        ->check(CLI::PositiveNumber);
    CLI::App& benchmark_suite =
        *app.add_subcommand("benchmark-suite", "Run the standard benchmark suite");
    CLI::App& self_test = *app.add_subcommand("self-test", "Run the built-in smoke test");

    app.parse(argc, argv);
    if (self_test) {
      janus::self_test();
    } else if (benchmark) {
      janus::benchmark(message_count);
    } else if (benchmark_suite) {
      bool header = true;
      for (const std::size_t count : {1000U, 10000U, 50000U}) {
        janus::benchmark(count, header);
        header = false;
      }
    } else if (import) {
      const janus::ProcessLock lock(import_options.state);
      janus::Syncer syncer(import_options.claude_root, import_options.codex_root,
                           import_options.state);
      const janus::Harness harness = import_path.filename().string().starts_with("rollout-")
                                         ? janus::Harness::codex
                                         : janus::Harness::claude;
      std::cout << syncer.import_one(import_path, harness).string() << '\n';
    } else if (sync) {
      const janus::ProcessLock lock(sync_options.state);
      janus::Syncer syncer(sync_options.claude_root, sync_options.codex_root, sync_options.state);
      std::cout << "sync changes: " << syncer.sync() << '\n';
    } else if (serve) {
      const janus::ProcessLock lock(serve_options.state);
      if (serve_options.daemonize) janus::daemonize_process();
      janus::Syncer syncer(serve_options.claude_root, serve_options.codex_root,
                           serve_options.state);
      std::signal(SIGINT, stop);
      std::signal(SIGTERM, stop);
      while (running) {
        try {
          syncer.sync();
        } catch (const std::exception& error) {
          std::cerr << "sync error: " << error.what() << '\n';
        }
        for (unsigned tick = 0; running && tick < serve_options.interval * 10; ++tick) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
    return 0;
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  } catch (const std::exception& error) {
    std::cerr << "janus: " << error.what() << '\n';
    return 1;
  }
}
