/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include "native_activity_guest_contract.h"

static void start(struct ActivityFixtureState* state) { ++state->starts; }
static void focus(struct ActivityFixtureState* state, int focused) { state->focused = focused; }
static void window(struct ActivityFixtureState* state, void* value) { state->window = value; }
static void rect(struct ActivityFixtureState* state, const struct ActivityFixtureRect* value) {
  state->rect[0] = value->left;
  state->rect[1] = value->top;
  state->rect[2] = value->right;
  state->rect[3] = value->bottom;
}
static void* save(struct ActivityFixtureState* state, size_t* length) {
  *length = sizeof(state->marker);
  return &state->marker;
}
void NativeActivityFixture_create(struct ActivityFixtureState* state, void* saved, size_t length) {
  state->marker = saved == 0 ? 0 : *(const uint64_t*)saved;
  state->input_size = length;
}
void NativeActivityFixture_callbacks(struct ActivityFixtureCallbacks* callbacks) {
  callbacks->start = start;
  callbacks->focus = focus;
  callbacks->window = window;
  callbacks->rect = rect;
  callbacks->save = save;
}
