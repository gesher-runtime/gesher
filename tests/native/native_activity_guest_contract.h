/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Original fixture storage. Callback signatures match NativeActivity's
// pointer/int/size_t and saved-state return shapes; this is not a UI surrogate.
struct ActivityFixtureState {
  uint64_t marker;
  int starts;
  int focused;
  void* window;
  int rect[4];
  size_t input_size;
};
struct ActivityFixtureRect { int left, top, right, bottom; };
struct ActivityFixtureCallbacks {
  void (*start)(struct ActivityFixtureState*);
  void (*focus)(struct ActivityFixtureState*, int);
  void (*window)(struct ActivityFixtureState*, void*);
  void (*rect)(struct ActivityFixtureState*, const struct ActivityFixtureRect*);
  void* (*save)(struct ActivityFixtureState*, size_t*);
};
