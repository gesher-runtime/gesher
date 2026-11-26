// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 The Gesher Authors
// Shared public-ABI checks for the real host adapter and NDK libc imports.
#ifndef REGEX_SCALING_REGEX
#define REGEX_SCALING_REGEX regex_t
#define REGEX_SCALING_MATCH regmatch_t
#define REGEX_SCALING_BEGIN rm_so
#define REGEX_SCALING_END rm_eo
#define REGEX_SCALING_ABI
#define REGEX_SCALING_CHECK check
#endif

static void test_regex_scaling_contracts(
    int (REGEX_SCALING_ABI *compile)(REGEX_SCALING_REGEX*, const char*, int),
    int (REGEX_SCALING_ABI *execute)(const REGEX_SCALING_REGEX*, const char*, size_t,
                                    REGEX_SCALING_MATCH*, int),
    void (REGEX_SCALING_ABI *release)(REGEX_SCALING_REGEX*)) {
  REGEX_SCALING_REGEX re;
  REGEX_SCALING_MATCH matches[4];
  char input[258];
  memset(&re, 0, sizeof(re));
  memset(input, 'a', 256);
  input[256] = 'b';
  input[257] = 0;
  int compiled = compile(&re, "(a?){32}a{32}b", 1);
  REGEX_SCALING_CHECK("regex nullable search compiles", compiled == 0);
  if (compiled == 0) {
    REGEX_SCALING_CHECK("regex merged states retain the earliest viable start",
        execute(&re, input, 1, matches, 0) == 0 &&
        matches[0].REGEX_SCALING_BEGIN == 192 && matches[0].REGEX_SCALING_END == 257);
    release(&re);
  }
  compiled = compile(&re, "(a|ab)", 1);
  REGEX_SCALING_CHECK("regex ambiguous prefix compiles", compiled == 0);
  if (compiled == 0) {
    for (size_t count = 1; count <= 4; ++count) {
      REGEX_SCALING_CHECK("regex observable match remains longest after early acceptance",
          execute(&re, "xab", count, matches, 0) == 0 &&
          matches[0].REGEX_SCALING_BEGIN == 1 && matches[0].REGEX_SCALING_END == 3);
      if (count > 1)
        REGEX_SCALING_CHECK("regex capture dissection keeps the longest alternative",
            matches[1].REGEX_SCALING_BEGIN == 1 && matches[1].REGEX_SCALING_END == 3);
      if (count > 2)
        REGEX_SCALING_CHECK("regex unused captures remain unmatched",
            matches[2].REGEX_SCALING_BEGIN == -1 && matches[2].REGEX_SCALING_END == -1);
    }
    release(&re);
  }
  for (int nosub = 0; nosub <= 1; ++nosub) {
    compiled = compile(&re, "a.*", 1 | (nosub ? 4 : 0));
    REGEX_SCALING_CHECK("regex boolean pattern compiles", compiled == 0);
    if (compiled != 0) continue;
    memset(matches, 0, sizeof(matches));
    matches[0].REGEX_SCALING_BEGIN = 1;
    matches[0].REGEX_SCALING_END = 257;
    matches[1].REGEX_SCALING_BEGIN = 71;
    matches[1].REGEX_SCALING_END = 93;
    REGEX_SCALING_CHECK("regex boolean STARTEND keeps caller bounds and capture guards",
        execute(&re, input, nosub ? 4 : 0, matches, 4) == 0 &&
        matches[0].REGEX_SCALING_BEGIN == 1 && matches[0].REGEX_SCALING_END == 257 &&
        matches[1].REGEX_SCALING_BEGIN == 71 && matches[1].REGEX_SCALING_END == 93);
    REGEX_SCALING_CHECK("regex boolean query still rejects a missing match",
        execute(&re, "xxxx", 0, NULL, 0) == 1);
    release(&re);
  }
  compiled = compile(&re, "\\(a\\)\\1", 4);
  REGEX_SCALING_CHECK("regex boolean backreference compiles", compiled == 0);
  if (compiled == 0) {
    REGEX_SCALING_CHECK("regex boolean backreference still requires the referenced bytes",
        execute(&re, "ab", 0, NULL, 0) == 1 && execute(&re, "aa", 0, NULL, 0) == 0);
    release(&re);
  }
  compiled = compile(&re, "a.b", 1);
  REGEX_SCALING_CHECK("regex binary bounded pattern compiles", compiled == 0);
  if (compiled == 0) {
    const char binary[4] = {'x', 'a', 0, 'b'};
    matches[0].REGEX_SCALING_BEGIN = 1;
    matches[0].REGEX_SCALING_END = 4;
    REGEX_SCALING_CHECK("regex shared closure preserves bounded embedded NUL matching",
        execute(&re, binary, 1, matches, 4) == 0 &&
        matches[0].REGEX_SCALING_BEGIN == 1 && matches[0].REGEX_SCALING_END == 4);
    release(&re);
  }
}

#undef REGEX_SCALING_REGEX
#undef REGEX_SCALING_MATCH
#undef REGEX_SCALING_BEGIN
#undef REGEX_SCALING_END
#undef REGEX_SCALING_ABI
#undef REGEX_SCALING_CHECK
