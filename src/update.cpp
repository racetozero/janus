#include "janus/update.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#include "janus/jsonl.hpp"

#ifndef JANUS_VERSION
#define JANUS_VERSION "0.0.1"
#endif

namespace janus {
namespace {

#ifdef NDEBUG
constexpr std::string_view repository = "racetozero/janus";

class TemporaryDirectory {
 public:
  TemporaryDirectory() : path_(fs::temp_directory_path() / ("janus-update-" + make_uuid())) {
    fs::create_directories(path_);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
  std::string result = "\"";
  for (const char character : value) {
    if (character == '"') result += '\\';
    result += character;
  }
  return result + '"';
#else
  std::string result = "'";
  for (const char character : value) {
    if (character == '\'') {
      result += "'\\''";
    } else {
      result += character;
    }
  }
  return result + '\'';
#endif
}

#ifdef _WIN32
std::string powershell_literal(std::string_view value) {
  std::string result = "'";
  for (const char character : value) {
    if (character == '\'') result += '\'';
    result += character;
  }
  return result + '\'';
}
#endif

std::string run_and_read(const std::string& command) {
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) throw std::runtime_error("could not start the update command");
  std::string output;
  std::array<char, 256> block{};
  while (std::fgets(block.data(), static_cast<int>(block.size()), pipe) != nullptr) {
    output += block.data();
  }
#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  if (status != 0) throw std::runtime_error("the update command failed");
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  return output;
}

std::string latest_tag() {
#ifdef _WIN32
  const std::string command =
      "powershell.exe -NoProfile -Command \"(Invoke-RestMethod -UseBasicParsing "
      "'https://api.github.com/repos/racetozero/janus/releases/latest').tag_name\"";
  const std::string tag = run_and_read(command);
#else
  const std::string command =
      "curl --proto '=https' --tlsv1.2 -fsSL "
      "https://api.github.com/repos/racetozero/janus/releases/latest | "
      "sed -n 's/.*\"tag_name\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p'";
  const std::string tag = run_and_read(command);
#endif
  const std::string version = tag.starts_with('v') ? tag.substr(1) : tag;
  if (version.empty() ||
      version.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz.-+") != std::string::npos) {
    throw std::runtime_error("the latest GitHub release has an invalid tag");
  }
  return version;
}

fs::path current_executable() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size == buffer.size()) {
    throw std::runtime_error("could not find the current Janus executable");
  }
  buffer.resize(size);
  return buffer;
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    throw std::runtime_error("could not find the current Janus executable");
  }
  return fs::canonical(buffer.c_str());
#else
  return fs::canonical("/proc/self/exe");
#endif
}

void download_installer(const std::string& version, const fs::path& destination) {
  const std::string url = "https://raw.githubusercontent.com/" + std::string(repository) + "/v" +
                          version +
#ifdef _WIN32
                          "/install.ps1";
  const std::string command = "powershell.exe -NoProfile -Command \"Invoke-WebRequest " +
                              powershell_literal(url) + " -OutFile " +
                              powershell_literal(destination.string()) + "\"";
#else
                          "/install.sh";
  const std::string command = "curl --proto '=https' --tlsv1.2 -fsSL " + shell_quote(url) + " -o " +
                              shell_quote(destination.string());
#endif
  run_and_read(command);
}

void set_update_environment(const std::string& version, const fs::path& install_directory) {
#ifdef _WIN32
  _putenv_s("JANUS_VERSION", version.c_str());
  _putenv_s("JANUS_INSTALL_DIR", install_directory.string().c_str());
  _putenv_s("JANUS_REPOSITORY", repository.data());
  _putenv_s("JANUS_RELEASES_URL", "");
#else
  setenv("JANUS_VERSION", version.c_str(), 1);
  setenv("JANUS_INSTALL_DIR", install_directory.c_str(), 1);
  setenv("JANUS_REPOSITORY", repository.data(), 1);
  unsetenv("JANUS_RELEASES_URL");
#endif
}

void run_installer(const fs::path& installer) {
#ifdef _WIN32
  const std::string command =
      "powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + shell_quote(installer.string());
#else
  const std::string command = "/bin/sh " + shell_quote(installer.string());
#endif
  run_and_read(command);
}

void replace_executable(const fs::path& source, const fs::path& destination) {
  const fs::path replacement = destination.parent_path() /
                               (".janus.update." + make_uuid() + destination.extension().string());
  fs::copy_file(source, replacement);
  struct RemoveReplacement {
    fs::path path;
    ~RemoveReplacement() {
      std::error_code error;
      fs::remove(path, error);
    }
  } remove_replacement{replacement};
#ifdef _WIN32
  const fs::path old = destination.string() + ".old";
  std::error_code ignored;
  fs::remove(old, ignored);
  if (!MoveFileExW(destination.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    throw std::runtime_error("could not move the current Janus executable");
  }
  if (!MoveFileExW(replacement.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    MoveFileExW(old.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING);
    throw std::runtime_error("could not install the new Janus executable");
  }
  MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
#else
  fs::permissions(replacement, fs::status(source).permissions());
  fs::rename(replacement, destination);
#endif
}
#endif

}  // namespace

void update() {
#ifndef NDEBUG
  throw std::runtime_error(
      "`janus update` is not available in debug builds; install a Janus release to use this "
      "command");
#else
  const std::string current = JANUS_VERSION;
  const std::string latest = latest_tag();
  if (latest == current) {
    std::cout << "janus v" << current << " is up to date.\n";
    return;
  }

  std::cout << "Updating janus from v" << current << " to v" << latest << "...\n";
  TemporaryDirectory temporary;
#ifdef _WIN32
  const fs::path installer = temporary.path() / "install.ps1";
  const fs::path staged = temporary.path() / "janus.exe";
#else
  const fs::path installer = temporary.path() / "install.sh";
  const fs::path staged = temporary.path() / "janus";
#endif
  download_installer(latest, installer);
  set_update_environment(latest, temporary.path());
  run_installer(installer);
  if (!fs::is_regular_file(staged)) {
    throw std::runtime_error("the Janus installer did not create a staged executable");
  }
  replace_executable(staged, current_executable());
  std::cout << "Updated janus to v" << latest << ". Restart Janus to use the new version.\n";
#endif
}

}  // namespace janus
