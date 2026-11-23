/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace gesher::windows::drop_wire {

constexpr uint32_t kMagic = 0x47534452;  // GSDR
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRelativeUnits = 4096;
constexpr uint32_t kMaxRoots = 128;
constexpr uint32_t kOpenDirectory = 1;
constexpr uint32_t kOpenMetadata = 2;

enum class Op : uint32_t { kAttach = 1, kPoll, kOpen, kRelease };
enum class Action : uint32_t { kStart = 1, kLocation, kDrop, kEnd, kEnter, kExit };

struct Request {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  Op op{};
  uint32_t flags = 0;
  uint64_t identity = 0;  // HWND on attach, lease on open and release.
  uint32_t item = 0;
  uint32_t units = 0;
};
static_assert(sizeof(Request) == 32);

struct Reply {
  uint32_t magic = kMagic;
  int32_t status = 0;  // Positive POSIX errno on failure.
  uint64_t handle = 0;
};
static_assert(sizeof(Reply) == 16);

struct Event {
  Action action{};
  float x = 0;
  float y = 0;
  uint32_t items = 0;
  uint64_t lease = 0;
};
static_assert(sizeof(Event) == 24);

struct Item {
  uint32_t name_units;
  uint32_t directory;
};
static_assert(sizeof(Item) == 8);

}  // namespace gesher::windows::drop_wire
