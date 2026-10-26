// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gesher::logging {
struct Entry {
  uint64_t timestamp = 0;  // Windows FILETIME, 100 ns since 1601 (UTC).
  uint32_t pid = 0, tid = 0;
  int32_t priority = 0, buffer = 0;
  std::string tag, text;
};
int Priority(char letter);
char PriorityLetter(int priority);
bool IsLoggable(int priority, const char* tag, size_t length, int fallback, int minimum);
// Structured log records use GESHER_LOG_FILE; stderr keeps its existing format.
void Record(int buffer, int priority, const char* tag, const char* text);
bool Read(std::vector<Entry>* entries, std::string* error);
bool Clear(std::string* error);
std::string Format(const Entry& entry, const std::string& format);
}  // namespace gesher::logging
