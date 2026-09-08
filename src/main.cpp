#include <CLI/CLI.hpp>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

#include "janus/sync.hpp"
#include "janus/update.hpp"

#ifndef JANUS_VERSION
#define JANUS_VERSION "0.0.1"
#endif

namespace {

volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

void add_paths(CLI::App& command, janus::Options& options) {
  command.add_option("--claude-root", options.claude_root, "Claude session root");
  command.add_option("--codex-root", options.codex_root, "Codex session root");
  command.add_option("--kiss-root", options.kiss_root, "KISS session root");
  command.add_option("--pi-root", options.pi_root, "Pi session root");
  command.add_option("--openclaw-root", options.openclaw_root, "OpenClaw agent root");
  command.add_option("--hermes-db", options.hermes_db, "Hermes state database");
  command.add_option("--opencode-db", options.opencode_db, "OpenCode database");
  command.add_option("--state", options.state, "Pair state file");
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{R"(     _
    (_) __ _ _ __  _   _ ___
    | |/ _` | '_ \| | | / __|
    | | (_| | | | | |_| \__ \
   _/ |\__,_|_| |_|\__,_|___/
  |__/

Move sessions between AI coding harnesses)",
               "janus"};
  try {
    app.set_version_flag("--version", "janus " JANUS_VERSION);

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

    CLI::App& update = *app.add_subcommand("update", "Update Janus from GitHub Releases");

    if (argc == 1) {
      std::cout << app.help();
      return 0;
    }

    app.parse(argc, argv);
    if (update) {
      janus::update();
    } else if (import) {
      const janus::ProcessLock lock(import_options.state);
      janus::Syncer syncer(import_options);
      const std::string path = import_path.string();
      const janus::Harness harness =
          import_path.filename().string().starts_with("rollout-") ? janus::Harness::codex
          : path.find(".kiss") != std::string::npos               ? janus::Harness::kiss
          : path.find(".pi") != std::string::npos                 ? janus::Harness::pi
                                                                  : janus::Harness::claude;
      std::cout << syncer.import_one(import_path, harness).string() << '\n';
    } else if (sync) {
      const janus::ProcessLock lock(sync_options.state);
      janus::Syncer syncer(sync_options);
      std::cout << "sync changes: " << syncer.sync() << '\n';
    } else if (serve) {
      const janus::ProcessLock lock(serve_options.state);
      if (serve_options.daemonize) janus::daemonize_process();
      janus::Syncer syncer(serve_options);
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
