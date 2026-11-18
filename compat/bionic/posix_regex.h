// Android16 r4 Bionic/NetBSD regex with a bounded private host adapter.
#pragma once
#include <climits>
#include "regex/adapter.h"

namespace gesher::bionic::posix_regex {
namespace netbsd {
#include "regex/port_macros.h"
#include "regex/regex_abi.h"
#include "regex/utils.h"
#include "regex/regex2.h"
#include "regex/cname.h"
#include "regex/regcomp.inc"
#include "regex/regfree.inc"
#include "regex/regexec.inc"
#include "regex/restore_macros.h"
}  // namespace netbsd

struct Program {
  Arena memory;
  netbsd::regex_t regex{};
  bool unsigned_char;
  explicit Program(bool byte_abi, size_t bytes = kMaxWorkBytes) : memory(bytes), unsigned_char(byte_abi) {}
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  // All compiler allocations belong to memory. Its destructor also cleans
  // partial trees if a work/depth/allocation limit unwinds upstream C code.
};

inline int Compile(Program& program, const char* pattern, const char* end, int flags,
                   size_t mb_cur_max, Limits limits = {kMaxWorkBytes, kMaxCompileSteps, kMaxDepth}) {
  Context context(program.memory, limits, program.unsigned_char, mb_cur_max);
  Scope scope(context);
  program.regex.re_endp = end;
  try {
    program.memory.SetLimit(limits.bytes);
    return netbsd::regcomp(&program.regex, pattern, flags);
  }
  catch (const ResourceLimit&) { return kRegEspace; }
}

inline int Execute(const Program& program, const char* input, size_t nmatch,
                   netbsd::regmatch_t* output, int flags, size_t mb_cur_max,
                   Limits limits = {}) {
  Arena memory(limits.bytes);
  if (program.regex.re_g->backrefs) limits.steps = std::min(limits.steps, kMaxBackrefSteps);
  Context context(memory, limits, program.unsigned_char, mb_cur_max);
  Scope scope(context);
  try {
    bool nosub = (program.regex.re_g->cflags & kRegNosub) != 0;
    size_t count = nosub || output == nullptr ? 0 : std::min(nmatch, program.regex.re_nsub + 1);
    size_t allocated = std::max(count, (flags & kRegStartend) != 0 ? size_t{1} : size_t{0});
    auto* matches = static_cast<netbsd::regmatch_t*>(Calloc(allocated, sizeof(netbsd::regmatch_t)));
    if (flags & kRegStartend) matches[0] = output[0];
    int result = netbsd::regexec(&program.regex, input, count, matches, flags);
    if (result != kRegOk) return result;
    if (!nosub && output != nullptr) {
      // Charge the whole publication first, so resource failures never expose
      // half-written caller captures (including unused trailing slots).
      Work(nmatch);
      for (size_t i = 0; i < nmatch; ++i) output[i] = i < count ? matches[i] : netbsd::regmatch_t{-1, -1};
    }
    return kRegOk;
  } catch (const ResourceLimit&) { return kRegEspace; }
}
}  // namespace gesher::bionic::posix_regex
