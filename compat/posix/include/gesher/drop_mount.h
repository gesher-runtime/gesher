/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::drop_mount {

inline constexpr wchar_t kPathPrefix[] = L"\\mnt\\gesher\\drop\\";
inline constexpr char kAndroidPrefix[] = "/mnt/gesher/drop/";

struct Event {
  uint32_t action = 0;
  float x = 0;
  float y = 0;
  std::vector<std::wstring> paths;
  std::wstring text;
  bool file_drag = false;
};

class Channel {
 public:
  static std::shared_ptr<Channel> Attach(HWND window);
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  bool Poll(Event* event);
  void Close();
  void Detach();
  bool Detached() const;
  void Release(uint64_t lease);

  // Open one item known to the guest channel; relative is scoped to that
  // item's retained Windows object on the host.
  int Open(uint64_t lease, uint32_t item, std::wstring_view relative, bool directory,
           bool metadata, HANDLE* file);

 private:
  class State;
  explicit Channel(HANDLE pipe);
  std::unique_ptr<State> state_;
};

struct Selection {
  bool matched = false;
  int error = 0;
  uint64_t alias = 0;
  std::wstring relative;
};

bool Enabled();
Selection Lookup(std::wstring_view mapped);
int Open(const Selection& selection, uint32_t flags, HANDLE* file);
bool Keep(std::wstring_view path);
void Release(std::wstring_view path);

}  // namespace gesher::drop_mount
