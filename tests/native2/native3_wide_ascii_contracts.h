// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

// The same public-call assertions run with actual guest wchar_t/mbstate_t,
// the pinned Android source, and the production shim's explicit ABI types.
template <class Word, class State, class Bounded, class Restartable,
          class Legacy, class Check>
void WideAsciiContracts(Bounded bounded, Restartable restartable, Legacy legacy,
                        Check check) {
  static_assert(sizeof(Word) == 4 && sizeof(State) == 8);
  auto initial = [] {
    State state{};
    std::memset(reinterpret_cast<unsigned char*>(&state) + 4, 0xa5, 4);
    return state;
  };
  auto unchanged = [&](const State& state) {
    State expected = initial();
    return std::memcmp(&state, &expected, sizeof(State)) == 0;
  };
  for (size_t n : {0u, 1u, 2u, 7u, 8u, 15u, 16u, 31u, 32u, 127u, 128u, 257u}) {
    std::vector<Word> words(n + 1);
    for (size_t i = 0; i < n; ++i) words[i] = static_cast<Word>(1 + i % 127);
    words[n] = 0;
    for (size_t cap : {size_t{0}, size_t{1}, n / 2, n, n + 1, n + 3}) {
      for (size_t limit : {size_t{0}, size_t{1}, n / 2, n, n + 1, n + 2}) {
        std::vector<unsigned char> output(cap + 4, 0xa5);
        const Word* cursor = words.data();
        State state = initial();
        errno = 17;
        size_t count = bounded(reinterpret_cast<char*>(output.data() + 2),
                               &cursor, limit, cap, &state);
        size_t expected = std::min({n, limit, cap});
        bool terminated = n < limit && n < cap;
        check(count == expected && cursor == (terminated ? nullptr : words.data() + expected),
              "ASCII bounded result and exact source position");
        bool bytes = output[0] == 0xa5 && output[1] == 0xa5;
        for (size_t i = 0; i < cap + 2; ++i) {
          unsigned char want = i < expected ? static_cast<unsigned char>(words[i])
                              : terminated && i == n ? 0 : 0xa5;
          bytes &= output[i + 2] == want;
        }
        check(bytes && unchanged(state) && errno == 17,
              "ASCII exact bytes, both guards, reserved state and errno");
        cursor = words.data();
        state = initial();
        check(bounded(nullptr, &cursor, limit, cap, &state) == std::min(n, limit) &&
                  cursor == words.data() && unchanged(state) && errno == 17,
              "ASCII measurement ignores output capacity and leaves source/state");
      }
      std::vector<unsigned char> output(cap + 4, 0xa5);
      const Word* cursor = words.data();
      State state = initial();
      size_t count = restartable(reinterpret_cast<char*>(output.data() + 2), &cursor, cap, &state);
      check(count == std::min(n, cap) && cursor == (n < cap ? nullptr : words.data() + count) &&
                unchanged(state), "ASCII restartable wrapper preserves the NUL boundary");
      std::vector<unsigned char> legacy_output(cap + 4, 0xa5);
      check(legacy(reinterpret_cast<char*>(legacy_output.data() + 2), words.data(), cap) == count &&
                legacy_output == output, "ASCII legacy wrapper uses the same bounded bytes");
    }
    for (size_t cap : {1u, 7u, 16u}) {
      const Word* cursor = words.data();
      State state = initial();
      size_t consumed = 0, calls = 0;
      bool bytes = true;
      while (cursor != nullptr && calls++ <= n + 1) {
        std::array<unsigned char, 18> output;
        output.fill(0xa5);
        size_t count = restartable(reinterpret_cast<char*>(output.data() + 1), &cursor, cap, &state);
        if (count > cap || count > n - consumed) { bytes = false; break; }
        for (size_t i = 0; i < count; ++i) bytes &= output[i + 1] == words[consumed + i];
        bytes &= output[0] == 0xa5 && output[cap + 1] == 0xa5;
        if (cursor == nullptr) bytes &= output[count + 1] == 0;
        consumed += count;
      }
      check(bytes && cursor == nullptr && consumed == n && unchanged(state),
            "ASCII repeated partial conversion and final NUL retirement");
    }
  }
  for (size_t pending = 0; pending < 4; ++pending) {
    for (size_t limit : {0u, 1u}) {
      for (size_t cap : {0u, 1u, 3u}) {
        const Word input[] = {'A', 0};
        const Word* cursor = input;
        State state = initial();
        reinterpret_cast<unsigned char*>(&state)[pending] = 0x80;
        std::array<char, 4> output = {'x', 'x', 'x', 'x'};
        errno = 17;
        check(bounded(output.data() + 1, &cursor, limit, cap, &state) == size_t(-1) &&
                  errno == 84 && cursor == input && unchanged(state) &&
                  output == std::array<char, 4>{'x', 'x', 'x', 'x'},
              "ASCII cannot bypass initial-state rejection, even with a zero bound");
      }
    }
  }
}
