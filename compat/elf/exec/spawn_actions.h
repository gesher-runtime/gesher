// Private launcher protocol for posix_spawn's ordered child setup.
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gesher::exec {

struct SpawnAction {
  int what, fd, new_fd, flags;
  uint32_t mode;
  std::string path;
};

struct SpawnRequest {
  bool search = false;
  std::string program;
  std::string search_path;
  std::vector<std::string> argv;
  std::vector<std::pair<int, int>> fd_flags;
  std::vector<SpawnAction> actions;
};

inline std::vector<std::string> EncodeSpawn(const SpawnRequest& request) {
  std::vector<std::string> out{"1", request.search ? "1" : "0", request.program, request.search_path,
                               std::to_string(request.argv.size())};
  out.insert(out.end(), request.argv.begin(), request.argv.end());
  out.push_back(std::to_string(request.fd_flags.size()));
  for (const auto& f : request.fd_flags) {
    out.push_back(std::to_string(f.first));
    out.push_back(std::to_string(f.second));
  }
  out.push_back(std::to_string(request.actions.size()));
  for (const SpawnAction& a : request.actions) {
    for (int n : {a.what, a.fd, a.new_fd, a.flags}) out.push_back(std::to_string(n));
    out.push_back(std::to_string(a.mode));
    out.push_back(a.path);
  }
  return out;
}

template <typename T>
bool SpawnNumber(const std::vector<std::string>& args, size_t* at, T* out) {
  if (*at >= args.size()) return false;
  const std::string& value = args[(*at)++];
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), *out);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

inline bool DecodeSpawn(const std::vector<std::string>& args, SpawnRequest* out) {
  if (args.size() < 8 || args[0] != "1" || (args[1] != "0" && args[1] != "1")) return false;
  SpawnRequest request;
  request.search = args[1] == "1";
  request.program = args[2];
  request.search_path = args[3];
  size_t at = 4, count = 0;
  if (!SpawnNumber(args, &at, &count) || count == 0 || count > args.size() - at) return false;
  request.argv.assign(args.begin() + at, args.begin() + at + count);
  at += count;
  if (!SpawnNumber(args, &at, &count) || count > (args.size() - at) / 2) return false;
  for (size_t i = 0; i < count; ++i) {
    int fd = -1, flags = 0;
    if (!SpawnNumber(args, &at, &fd) || fd < 0 || !SpawnNumber(args, &at, &flags) ||
        (flags != 0 && flags != 1)) return false;
    request.fd_flags.emplace_back(fd, flags);
  }
  if (!SpawnNumber(args, &at, &count) || count > (args.size() - at) / 6) return false;
  for (size_t i = 0; i < count; ++i) {
    SpawnAction a{};
    if (!SpawnNumber(args, &at, &a.what) || a.what < 0 || a.what > 4 ||
        !SpawnNumber(args, &at, &a.fd) || !SpawnNumber(args, &at, &a.new_fd) ||
        !SpawnNumber(args, &at, &a.flags) || !SpawnNumber(args, &at, &a.mode)) return false;
    a.path = args[at++];
    request.actions.push_back(std::move(a));
  }
  if (at != args.size()) return false;
  *out = std::move(request);
  return true;
}

// Runs in the new launcher process only. 127 on the first failed non-close
// action, as pinned bionic spawn.cpp; zero after successful child setup.
int ApplySpawnActions(const SpawnRequest& request);

// Optional diagnostics retain the original child127 contract.
int SpawnFailure(const char* operation, const char* path = "", int fd = -1,
                 int new_fd = -1, int action = -1);

}  // namespace gesher::exec
