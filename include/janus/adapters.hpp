#pragma once

#include <functional>

#include "janus/jsonl.hpp"

namespace janus {

using MessageFunction = std::function<void(const Message&)>;

Session scan_session(const fs::path& path, Harness harness, const MessageFunction& on_message = {});
void append_claude(Appender& output, Session& target, const Message& message);
void append_codex(Appender& output, Session& target, const Message& message);
fs::path create_peer(const fs::path& root, const fs::path& source_path, Harness source_harness);
std::size_t copy_missing(const fs::path& source_path, Harness source_harness,
                         const fs::path& target_path, Harness target_harness);

}  // namespace janus
