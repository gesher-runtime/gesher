// POSIX/Bionic-compatible regular expression engine for compat/bionic.
// Supports BRE (REG_BASIC) and ERE (REG_EXTENDED), REG_ICASE, REG_NOSUB,
// REG_NEWLINE (NetBSD/Android per-byte LF anchor/negated-class semantics),
// REG_PEND, REG_NOTBOL, REG_NOTEOL, and REG_STARTEND, with linear-time
// leftmost-longest whole-match NFA execution for backreference-free patterns.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "posix_regex_cnames.h"

namespace gesher::bionic::posix_regex {

constexpr int kRegExtended = 1;
constexpr int kRegIcase = 2;
constexpr int kRegNosub = 4;
constexpr int kRegNewline = 8;
constexpr int kRegPend = 32;

constexpr int kRegNotbol = 1;
constexpr int kRegNoteol = 2;
constexpr int kRegStartend = 4;

constexpr int kRegOk = 0;
constexpr int kRegNomatch = 1;
constexpr int kRegBadpat = 2;
constexpr int kRegEcollate = 3;
constexpr int kRegEctype = 4;
constexpr int kRegEescape = 5;
constexpr int kRegEsubreg = 6;
constexpr int kRegEbrack = 7;
constexpr int kRegEparen = 8;
constexpr int kRegEbrace = 9;
constexpr int kRegBadbr = 10;
constexpr int kRegErange = 11;
constexpr int kRegEspace = 12;
constexpr int kRegBadrpt = 13;
constexpr int kRegEmpty = 14;
constexpr int kRegInvarg = 16;

constexpr size_t kMaxDupCount = 255;
constexpr size_t kMaxInsns = 65536;
constexpr size_t kMaxBackrefSteps = 2000000;
constexpr size_t kMaxParseDepth = 256;
constexpr size_t kMaxWorkBytes = 64 * 1024 * 1024;

struct CharClass {
  uint64_t bits[4]{};

  void Set(unsigned char c) { bits[c >> 6] |= (uint64_t{1} << (c & 63)); }
  void Clear(unsigned char c) { bits[c >> 6] &= ~(uint64_t{1} << (c & 63)); }
  bool Test(unsigned char c) const { return (bits[c >> 6] & (uint64_t{1} << (c & 63))) != 0; }
};

enum class Op : uint8_t {
  kByte,
  kAny,
  kClass,
  kBol,
  kEol,
  kWordBeg,
  kWordEnd,
  kSave,
  kBackref,
  kSplit,
  kJump,
  kMatch,
};

struct Insn {
  Op op = Op::kMatch;
  uint8_t byte_val = 0;
  int32_t arg = 0;
  int32_t out1 = -1;
  int32_t out2 = -1;
};

struct Program {
  std::vector<Insn> insns;
  std::vector<CharClass> classes;
  int32_t entry = -1;
  size_t nsub = 0;
  bool icase = false;
  bool nosub = false;
  bool newline = false;
  bool has_backref = false;
};

inline unsigned char FoldLower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

inline unsigned char FoldUpper(unsigned char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - ('a' - 'A')) : c;
}

inline bool IsWordByte(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline bool AddPosixNamedClass(const std::string& name, CharClass& cls) {
  auto add_pred = [&](auto pred) {
    for (int i = 0; i < 256; ++i) {
      if (pred(i)) cls.Set(static_cast<unsigned char>(i));
    }
  };
  auto is_upper = [](int c) { return c >= 'A' && c <= 'Z'; };
  auto is_lower = [](int c) { return c >= 'a' && c <= 'z'; };
  auto is_alpha = [&](int c) { return is_upper(c) || is_lower(c); };
  auto is_digit = [](int c) { return c >= '0' && c <= '9'; };
  auto is_alnum = [&](int c) { return is_alpha(c) || is_digit(c); };
  auto is_space = [](int c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  auto is_cntrl = [](int c) { return (c >= 0 && c <= 31) || c == 127; };
  auto is_print = [](int c) { return c >= 32 && c <= 126; };
  auto is_graph = [](int c) { return c >= 33 && c <= 126; };
  auto is_punct = [&](int c) { return is_graph(c) && !is_alnum(c); };
  auto is_xdigit = [&](int c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  if (name == "alnum") add_pred(is_alnum);
  else if (name == "alpha") add_pred(is_alpha);
  else if (name == "blank") add_pred([](int c) { return c == ' ' || c == '\t'; });
  else if (name == "cntrl") add_pred(is_cntrl);
  else if (name == "digit") add_pred(is_digit);
  else if (name == "graph") add_pred(is_graph);
  else if (name == "lower") add_pred(is_lower);
  else if (name == "print") add_pred(is_print);
  else if (name == "punct") add_pred(is_punct);
  else if (name == "space") add_pred(is_space);
  else if (name == "upper") add_pred(is_upper);
  else if (name == "xdigit") add_pred(is_xdigit);
  else return false;
  return true;
}

class Compiler {
 public:
  Compiler(const std::string& pat, bool extended, bool icase, bool nosub, bool newline)
      : pat_(pat), extended_(extended) {
    prog_.icase = icase;
    prog_.nosub = nosub;
    prog_.newline = newline;
  }

  int Compile(Program& out_prog) {
    Frag root;
    int err = ParseExpr(root, /*stop_on_rparen=*/false);
    if (err != kRegOk) return err;
    if (pos_ != pat_.size()) {
      return (!extended_ && pos_ + 1 < pat_.size() && pat_[pos_] == '\\' && pat_[pos_ + 1] == ')')
                 ? kRegEparen
                 : kRegBadpat;
    }
    int32_t match_pc = Emit(Op::kMatch, 0, 0);
    if (match_pc < 0) return kRegEspace;
    Patch(root.exits, match_pc);
    prog_.entry = root.start;
    if (prog_.insns.size() > kMaxInsns) return kRegEspace;
    out_prog = std::move(prog_);
    return kRegOk;
  }

 private:
  struct Frag {
    int32_t start = -1;
    std::vector<int32_t> exits;
    bool repeatable = false;
    bool elided = false;
  };

  int32_t Emit(Op op, uint8_t b = 0, int32_t arg = 0, int32_t out1 = -1, int32_t out2 = -1) {
    if (prog_.insns.size() >= kMaxInsns) return -1;
    int32_t pc = static_cast<int32_t>(prog_.insns.size());
    prog_.insns.push_back(Insn{op, b, arg, out1, out2});
    return pc;
  }

  void Patch(const std::vector<int32_t>& exits, int32_t target) {
    for (int32_t pc : exits) {
      if (prog_.insns[pc].op == Op::kSplit && prog_.insns[pc].out2 == -1) {
        prog_.insns[pc].out2 = target;
      } else {
        prog_.insns[pc].out1 = target;
      }
    }
  }

  int ParseExpr(Frag& out, bool stop_on_rparen) {
    Frag first;
    int err = ParseBranch(first, stop_on_rparen);
    if (err != kRegOk) return err;

    if (!extended_) {
      out = std::move(first);
      return kRegOk;
    }

    std::vector<Frag> branches;
    branches.push_back(std::move(first));
    while (pos_ < pat_.size() && pat_[pos_] == '|') {
      ++pos_;
      Frag next;
      err = ParseBranch(next, stop_on_rparen);
      if (err != kRegOk) return err;
      branches.push_back(std::move(next));
    }
    if (branches.size() == 1) {
      out = std::move(branches[0]);
      return kRegOk;
    }

    int32_t tail_split = branches.back().start;
    std::vector<int32_t> all_exits = std::move(branches.back().exits);
    for (size_t i = branches.size() - 1; i > 0; --i) {
      int32_t sp = Emit(Op::kSplit, 0, 0, branches[i - 1].start, tail_split);
      if (sp < 0) return kRegEspace;
      tail_split = sp;
      all_exits.insert(all_exits.end(), branches[i - 1].exits.begin(), branches[i - 1].exits.end());
    }
    out.start = tail_split;
    out.exits = std::move(all_exits);
    out.repeatable = true;
    return kRegOk;
  }

  bool AtStop(bool stop_on_rparen) const {
    if (pos_ >= pat_.size()) return true;
    if (extended_) {
      if (pat_[pos_] == '|') return true;
      if (stop_on_rparen && pat_[pos_] == ')') return true;
    } else {
      if (stop_on_rparen && pos_ + 1 < pat_.size() && pat_[pos_] == '\\' && pat_[pos_ + 1] == ')') {
        return true;
      }
    }
    return false;
  }

  Frag MakeEmptyFrag() {
    int32_t j = Emit(Op::kJump);
    Frag f;
    f.start = j;
    if (j >= 0) f.exits.push_back(j);
    f.repeatable = false;
    f.elided = true;
    return f;
  }

  int ParseBranch(Frag& out, bool stop_on_rparen) {
    bool has_prev = false;
    bool at_piece_start = true;
    Frag seq;
    while (!AtStop(stop_on_rparen)) {
      Frag piece;
      int err = ParsePiece(piece, at_piece_start, stop_on_rparen);
      if (err != kRegOk) return err;
      at_piece_start = false;
      if (!has_prev) {
        seq = std::move(piece);
        has_prev = true;
      } else {
        Patch(seq.exits, piece.start);
        seq.exits = std::move(piece.exits);
        seq.elided = seq.elided && piece.elided;
      }
    }
    // NetBSD permits an explicitly empty group, but rejects an empty expression
    // or alternative, including one erased entirely by a {0} repetition.
    if (!has_prev || seq.elided) return kRegEmpty;
    out = std::move(seq);
    return kRegOk;
  }

  // Re-parses a saved pattern slice `[start_pos, end_pos)` for bound repetition `{m,n}`.
  int ParseAtomSlice(size_t start_pos, size_t end_pos, bool at_piece_start, size_t base_nsub,
                     size_t max_nsub, Frag& out) {
    size_t saved_pos = pos_;
    const auto saved_closed = closed_groups_;
    pos_ = start_pos;
    prog_.nsub = base_nsub;
    int err = ParseAtom(out, at_piece_start, /*stop_on_rparen=*/true);
    if (err == kRegOk && pos_ != end_pos) err = kRegBadpat;
    pos_ = saved_pos;
    closed_groups_ = saved_closed;
    prog_.nsub = std::max(prog_.nsub, max_nsub);
    return err;
  }

  int ParsePiece(Frag& out, bool at_piece_start, bool stop_on_rparen) {
    size_t atom_start = pos_;
    size_t base_nsub = prog_.nsub;
    Frag atom;
    int err = ParseAtom(atom, at_piece_start, stop_on_rparen);
    if (err != kRegOk) return err;
    size_t atom_end = pos_;
    size_t max_nsub = prog_.nsub;

    if (extended_) {
      while (pos_ < pat_.size()) {
        char ch = pat_[pos_];
        if (ch == '*' || ch == '+' || ch == '?') {
          if (!atom.repeatable) return kRegBadrpt;
          ++pos_;
          if (pos_ < pat_.size() && (pat_[pos_] == '*' || pat_[pos_] == '+' || pat_[pos_] == '?' || pat_[pos_] == '{')) {
            return kRegBadrpt;
          }
          return ApplySimpleRep(atom, ch, out);
        }
        if (ch == '{') {
          if (!atom.repeatable) return kRegBadrpt;
          if (pos_ + 1 == pat_.size() ||
              ((pat_[pos_ + 1] < '0' || pat_[pos_ + 1] > '9') && pat_[pos_ + 1] != ',')) {
            return kRegBadrpt;
          }
          size_t min_rep = 0;
          size_t max_rep = 0;
          bool unbounded = false;
          err = ParseBounds(/*basic_br=*/false, min_rep, max_rep, unbounded);
          if (err != kRegOk) return err;
          if (pos_ < pat_.size() && (pat_[pos_] == '*' || pat_[pos_] == '+' || pat_[pos_] == '?' || pat_[pos_] == '{')) {
            return kRegBadrpt;
          }
          return ApplyBoundedRep(atom, atom_start, atom_end, at_piece_start, base_nsub, max_nsub,
                                 min_rep, max_rep, unbounded, out);
        }
        break;
      }
    } else {
      if (pos_ < pat_.size() && pat_[pos_] == '*') {
        if (atom.repeatable) {
          ++pos_;
          if (pos_ < pat_.size() && pat_[pos_] == '*') return kRegBadrpt;
          return ApplySimpleRep(atom, '*', out);
        }
      } else if (pos_ + 1 < pat_.size() && pat_[pos_] == '\\' && pat_[pos_ + 1] == '{') {
        if (!atom.repeatable) return kRegBadrpt;
        size_t min_rep = 0;
        size_t max_rep = 0;
        bool unbounded = false;
        err = ParseBounds(/*basic_br=*/true, min_rep, max_rep, unbounded);
        if (err != kRegOk) return err;
        return ApplyBoundedRep(atom, atom_start, atom_end, at_piece_start, base_nsub, max_nsub,
                               min_rep, max_rep, unbounded, out);
      }
    }
    out = std::move(atom);
    return kRegOk;
  }

  int ApplySimpleRep(Frag& atom, char op, Frag& out) {
    if (op == '*') {
      int32_t sp = Emit(Op::kSplit, 0, 0, atom.start, -1);
      if (sp < 0) return kRegEspace;
      Patch(atom.exits, sp);
      out.start = sp;
      out.exits = {sp};
      out.repeatable = false;
      return kRegOk;
    }
    if (op == '+') {
      int32_t sp = Emit(Op::kSplit, 0, 0, atom.start, -1);
      if (sp < 0) return kRegEspace;
      Patch(atom.exits, sp);
      out.start = atom.start;
      out.exits = {sp};
      out.repeatable = false;
      return kRegOk;
    }
    // '?'
    int32_t sp = Emit(Op::kSplit, 0, 0, atom.start, -1);
    if (sp < 0) return kRegEspace;
    out.start = sp;
    out.exits = std::move(atom.exits);
    out.exits.push_back(sp);
    out.repeatable = false;
    return kRegOk;
  }

  int ApplyBoundedRep(Frag& first_atom, size_t atom_start, size_t atom_end, bool at_piece_start,
                      size_t base_nsub, size_t max_nsub, size_t min_rep, size_t max_rep,
                      bool unbounded, Frag& out) {
    // Discard/skip unused first_atom if min_rep == 0 && !unbounded && max_rep == 0.
    // To keep group numbering/NFA offsets clean, reuse first_atom as copy 0 when needed.
    std::vector<Frag> req;
    std::vector<Frag> opt;
    bool used_first = false;

    auto next_copy = [&](Frag& dst) -> int {
      if (!used_first) {
        used_first = true;
        dst = std::move(first_atom);
        return kRegOk;
      }
      return ParseAtomSlice(atom_start, atom_end, at_piece_start, base_nsub, max_nsub, dst);
    };

    if (min_rep == 0 && !unbounded && max_rep == 0) {
      // Truncate first_atom instructions if no later jumps point into it: safe to bypass via Jump.
      out = MakeEmptyFrag();
      if (out.start < 0) return kRegEspace;
      return kRegOk;
    }

    for (size_t i = 0; i < min_rep; ++i) {
      Frag c;
      int err = next_copy(c);
      if (err != kRegOk) return err;
      req.push_back(std::move(c));
    }

    if (unbounded) {
      Frag tail;
      int err = next_copy(tail);
      if (err != kRegOk) return err;
      Frag star;
      err = ApplySimpleRep(tail, '*', star);
      if (err != kRegOk) return err;
      opt.push_back(std::move(star));
    } else {
      for (size_t i = min_rep; i < max_rep; ++i) {
        Frag copy;
        int err = next_copy(copy);
        if (err != kRegOk) return err;
        Frag q;
        err = ApplySimpleRep(copy, '?', q);
        if (err != kRegOk) return err;
        opt.push_back(std::move(q));
      }
    }

    Frag combined;
    bool has_combined = false;
    auto append = [&](Frag& f) {
      if (!has_combined) {
        combined = std::move(f);
        has_combined = true;
      } else {
        Patch(combined.exits, f.start);
        combined.exits = std::move(f.exits);
      }
    };
    for (auto& f : req) append(f);
    for (auto& f : opt) append(f);
    combined.repeatable = false;
    out = std::move(combined);
    return kRegOk;
  }

  int ParseBounds(bool basic_br, size_t& min_rep, size_t& max_rep, bool& unbounded) {
    pos_ += basic_br ? 2 : 1;
    auto at_close = [&]() -> bool {
      if (basic_br) return pos_ + 1 < pat_.size() && pat_[pos_] == '\\' && pat_[pos_ + 1] == '}';
      return pos_ < pat_.size() && pat_[pos_] == '}';
    };
    auto parse_num = [&](size_t& val) -> int {
      if (pos_ >= pat_.size() || pat_[pos_] < '0' || pat_[pos_] > '9') return kRegBadbr;
      val = 0;
      while (pos_ < pat_.size() && pat_[pos_] >= '0' && pat_[pos_] <= '9') {
        val = val * 10 + static_cast<size_t>(pat_[pos_++] - '0');
        if (val > kMaxDupCount) return kRegBadbr;
      }
      return kRegOk;
    };
    int err = parse_num(min_rep);
    if (err != kRegOk) return (pos_ >= pat_.size()) ? kRegEbrace : err;
    unbounded = false;
    max_rep = min_rep;
    if (pos_ < pat_.size() && pat_[pos_] == ',') {
      ++pos_;
      if (pos_ >= pat_.size()) return kRegEbrace;
      if (at_close()) {
        unbounded = true;
      } else {
        err = parse_num(max_rep);
        if (err != kRegOk) return (pos_ >= pat_.size()) ? kRegEbrace : err;
        if (max_rep < min_rep) return kRegBadbr;
      }
    }
    if (pos_ >= pat_.size()) return kRegEbrace;
    if (!at_close()) {
      // Scan ahead for closing brace to distinguish REG_EBRACE vs REG_BADBR.
      for (size_t p = pos_; p < pat_.size(); ++p) {
        if (!basic_br && pat_[p] == '}') return kRegBadbr;
        if (basic_br && pat_[p] == '\\' && p + 1 < pat_.size() && pat_[p + 1] == '}') {
          return kRegBadbr;
        }
      }
      return kRegEbrace;
    }
    pos_ += basic_br ? 2 : 1;
    return kRegOk;
  }

  int ParseAtom(Frag& out, bool at_piece_start, bool stop_on_rparen) {
    if (pos_ >= pat_.size()) return kRegBadpat;
    char ch = pat_[pos_];

    if (extended_) {
      if (ch == '*' || ch == '+' || ch == '?' || ch == '{') return kRegBadrpt;
      if (ch == ')') {
        if (stop_on_rparen) return kRegBadpat;
        // NetBSD ERE treats unmatched ')' outside '(' as literal ')'.
        ++pos_;
        return MakeByte(static_cast<uint8_t>(')'), out);
      }
      if (ch == '(') {
        return ParseGroup(/*basic_group=*/false, out);
      }
      if (ch == '^') {
        ++pos_;
        int32_t pc = Emit(Op::kBol);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
      if (ch == '$') {
        ++pos_;
        int32_t pc = Emit(Op::kEol);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
    } else {
      // BRE rules: '^' is anchor at start or immediately after '\(',
      // '$' is anchor at end or immediately before '\)'. '*' at start or after '^' is literal.
      if (ch == '^' && at_piece_start) {
        ++pos_;
        int32_t pc = Emit(Op::kBol);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
      if (ch == '$' && (pos_ + 1 == pat_.size() ||
                        (stop_on_rparen && pos_ + 2 < pat_.size() &&
                         pat_[pos_ + 1] == '\\' && pat_[pos_ + 2] == ')'))) {
        ++pos_;
        int32_t pc = Emit(Op::kEol);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
      if (ch == '\\') {
        if (pos_ + 1 >= pat_.size()) return kRegEescape;
        char next = pat_[pos_ + 1];
        if (next == '(') return ParseGroup(/*basic_group=*/true, out);
        if (next == ')') return kRegEparen;
        if (next == '{') return kRegBadrpt;
        if (next >= '1' && next <= '9') {
          size_t idx = static_cast<size_t>(next - '0');
          if (!closed_groups_[idx]) return kRegEsubreg;
          pos_ += 2;
          prog_.has_backref = true;
          int32_t pc = Emit(Op::kBackref, 0, static_cast<int32_t>(idx));
          if (pc < 0) return kRegEspace;
          out = Frag{pc, {pc}, true};
          return kRegOk;
        }
      }
    }

    if (ch == '.') {
      ++pos_;
      int32_t pc = Emit(Op::kAny);
      if (pc < 0) return kRegEspace;
      out = Frag{pc, {pc}, true};
      return kRegOk;
    }
    if (ch == '[') {
      return ParseBracket(out);
    }
    if (ch == '\\') {
      if (pos_ + 1 >= pat_.size()) return kRegEescape;
      char next = pat_[pos_ + 1];
      if (next == '<') {
        pos_ += 2;
        int32_t pc = Emit(Op::kWordBeg);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
      if (next == '>') {
        pos_ += 2;
        int32_t pc = Emit(Op::kWordEnd);
        if (pc < 0) return kRegEspace;
        out = Frag{pc, {pc}, false};
        return kRegOk;
      }
      if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
          next == '\'' || next == '`') {
        return kRegEescape;
      }
      pos_ += 2;
      return MakeByte(static_cast<uint8_t>(next), out);
    }

    ++pos_;
    return MakeByte(static_cast<uint8_t>(ch), out);
  }

  int ParseGroup(bool basic_group, Frag& out) {
    if (parse_depth_ >= kMaxParseDepth) return kRegEspace;
    struct DepthScope {
      size_t& depth;
      explicit DepthScope(size_t& value) : depth(value) { ++depth; }
      ~DepthScope() { --depth; }
    } depth_scope(parse_depth_);
    pos_ += basic_group ? 2 : 1;
    ++prog_.nsub;
    size_t gid = prog_.nsub;
    if (gid < closed_groups_.size()) closed_groups_[gid] = false;
    // NOSUB suppresses public results, not captures used by a later backreference.
    int32_t open_pc = Emit(Op::kSave, 0, static_cast<int32_t>(2 * gid));
    if (open_pc < 0) return kRegEspace;

    Frag inner;
    const bool empty_group = basic_group
                                 ? pos_ + 1 < pat_.size() && pat_[pos_] == '\\' &&
                                       pat_[pos_ + 1] == ')'
                                 : pos_ < pat_.size() && pat_[pos_] == ')';
    if (empty_group) {
      inner = MakeEmptyFrag();
      if (inner.start < 0) return kRegEspace;
    } else {
      if (pos_ == pat_.size()) return kRegEparen;
      int err = ParseExpr(inner, /*stop_on_rparen=*/true);
      if (err != kRegOk) return err;
    }

    if (basic_group) {
      if (pos_ + 1 >= pat_.size() || pat_[pos_] != '\\' || pat_[pos_ + 1] != ')') {
        return kRegEparen;
      }
      pos_ += 2;
    } else {
      if (pos_ >= pat_.size() || pat_[pos_] != ')') return kRegEparen;
      ++pos_;
    }

    if (gid < closed_groups_.size()) closed_groups_[gid] = true;
    int32_t close_pc = Emit(Op::kSave, 0, static_cast<int32_t>(2 * gid + 1));
    if (close_pc < 0) return kRegEspace;

    prog_.insns[open_pc].out1 = inner.start;
    Patch(inner.exits, close_pc);
    out.start = open_pc;
    out.exits = {close_pc};
    out.repeatable = true;
    return kRegOk;
  }

  int ParseCollatingElement(char kind, uint8_t& value) {
    const size_t start = pos_;
    while (pos_ + 1 < pat_.size() && !(pat_[pos_] == kind && pat_[pos_ + 1] == ']')) {
      ++pos_;
    }
    if (pos_ + 1 >= pat_.size()) return kRegEbrack;
    const std::string name = pat_.substr(start, pos_ - start);
    pos_ += 2;
    for (const auto& entry : detail::cnames) {
      if (entry.name == nullptr) break;
      if (name == entry.name) {
        value = static_cast<uint8_t>(entry.code);
        return kRegOk;
      }
    }
    if (name.size() != 1) return kRegEcollate;
    value = static_cast<uint8_t>(name[0]);
    return kRegOk;
  }

  int ParseBracket(Frag& out) {
    if (pat_.compare(pos_, 7, "[[:<:]]") == 0 || pat_.compare(pos_, 7, "[[:>:]]") == 0) {
      const Op op = pat_[pos_ + 3] == '<' ? Op::kWordBeg : Op::kWordEnd;
      pos_ += 7;
      const int32_t pc = Emit(op);
      if (pc < 0) return kRegEspace;
      out = Frag{pc, {pc}, true};
      return kRegOk;
    }
    ++pos_;
    if (pos_ >= pat_.size()) return kRegEbrack;
    const bool negated = pat_[pos_] == '^';
    if (negated) ++pos_;
    CharClass cls;
    auto add_single = [&](uint8_t c) {
      cls.Set(c);
      if (prog_.icase) {
        cls.Set(FoldLower(c));
        cls.Set(FoldUpper(c));
      }
    };
    auto parse_symbol = [&](uint8_t& c) -> int {
      if (pos_ == pat_.size()) return kRegEbrack;
      if (pos_ + 1 < pat_.size() && pat_[pos_] == '[' && pat_[pos_ + 1] == '.') {
        pos_ += 2;
        return ParseCollatingElement('.', c);
      }
      c = static_cast<uint8_t>(pat_[pos_++]);
      return kRegOk;
    };
    // ']' and '-' are literal at the beginning of a bracket expression.
    if (pos_ < pat_.size() && (pat_[pos_] == ']' || pat_[pos_] == '-')) {
      add_single(static_cast<uint8_t>(pat_[pos_++]));
    }
    while (pos_ < pat_.size() && pat_[pos_] != ']' &&
           !(pat_[pos_] == '-' && pos_ + 1 < pat_.size() && pat_[pos_ + 1] == ']')) {
      if (pat_[pos_] == '-') return kRegErange;
      if (pos_ + 1 < pat_.size() && pat_[pos_] == '[' && pat_[pos_ + 1] == ':') {
        pos_ += 2;
        if (pos_ == pat_.size()) return kRegEbrack;
        const size_t start = pos_;
        while (pos_ < pat_.size() &&
               ((pat_[pos_] >= 'a' && pat_[pos_] <= 'z') ||
                (pat_[pos_] >= 'A' && pat_[pos_] <= 'Z'))) {
          ++pos_;
        }
        CharClass named;
        if (!AddPosixNamedClass(pat_.substr(start, pos_ - start), named)) return kRegEctype;
        if (pos_ == pat_.size()) return kRegEbrack;
        if (pos_ + 1 >= pat_.size() || pat_[pos_] != ':' || pat_[pos_ + 1] != ']') {
          return kRegEctype;
        }
        pos_ += 2;
        for (int c = 0; c < 256; ++c) {
          if (named.Test(static_cast<uint8_t>(c))) add_single(static_cast<uint8_t>(c));
        }
        continue;
      }
      if (pos_ + 1 < pat_.size() && pat_[pos_] == '[' && pat_[pos_ + 1] == '=') {
        pos_ += 2;
        if (pos_ == pat_.size()) return kRegEbrack;
        if (pat_[pos_] == '-' || pat_[pos_] == ']') return kRegEcollate;
        uint8_t c;
        const int err = ParseCollatingElement('=', c);
        if (err != kRegOk) return err;
        add_single(c);
        continue;
      }
      uint8_t first;
      int err = parse_symbol(first);
      if (err != kRegOk) return err;
      uint8_t last = first;
      if (pos_ + 1 < pat_.size() && pat_[pos_] == '-' && pat_[pos_ + 1] != ']') {
        ++pos_;
        err = parse_symbol(last);
        if (err != kRegOk) return err;
      }
      if (first > last) return kRegErange;
      for (int c = first; c <= last; ++c) add_single(static_cast<uint8_t>(c));
    }
    if (pos_ < pat_.size() && pat_[pos_] == '-') {
      add_single('-');
      ++pos_;
    }
    if (pos_ == pat_.size() || pat_[pos_] != ']') return kRegEbrack;
    ++pos_;
    if (negated) {
      for (int i = 0; i < 4; ++i) cls.bits[i] = ~cls.bits[i];
      if (prog_.newline) cls.Clear('\n');
    }
    const int32_t cid = static_cast<int32_t>(prog_.classes.size());
    prog_.classes.push_back(cls);
    const int32_t pc = Emit(Op::kClass, 0, cid);
    if (pc < 0) return kRegEspace;
    out = Frag{pc, {pc}, true};
    return kRegOk;
  }

  int MakeByte(uint8_t b, Frag& out) {
    uint8_t stored = prog_.icase ? FoldLower(b) : b;
    int32_t pc = Emit(Op::kByte, stored);
    if (pc < 0) return kRegEspace;
    out = Frag{pc, {pc}, true};
    return kRegOk;
  }

  const std::string& pat_;
  bool extended_;
  size_t pos_ = 0;
  std::array<bool, 10> closed_groups_{};
  size_t parse_depth_ = 0;
  Program prog_;
};

class Matcher {
 public:
  Matcher(const Program& prog, const char* base, const char* begin, const char* end, int eflags,
          size_t nmatch)
      : prog_(prog),
        base_(base),
        begin_(begin),
        len_(static_cast<size_t>(end - begin)),
        eflags_(eflags),
        track_caps_(!prog.nosub && nmatch > 1 && prog.nsub > 0),
        num_slots_(track_caps_ ? 2 * (prog.nsub + 1) : 0) {}

  int Execute(std::vector<int32_t>& out_caps, int32_t& out_start, int32_t& out_end) {
    if (len_ > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return kRegEspace;
    if (prog_.has_backref) {
      return ExecuteBackref(out_caps, out_start, out_end);
    }
    return ExecuteNfa(out_caps, out_start, out_end);
  }

 private:
  struct Thread {
    int32_t pc = 0;
    int32_t start = 0;
    std::vector<int32_t> caps;
  };

  bool IsBol(size_t pos) const {
    if (pos == 0) {
      if ((eflags_ & kRegNotbol) == 0) return true;
      return prog_.newline && begin_ > base_ && begin_[-1] == '\n';
    }
    return prog_.newline && static_cast<unsigned char>(begin_[pos - 1]) == '\n';
  }

  bool IsEol(size_t pos) const {
    if (pos == len_) return (eflags_ & kRegNoteol) == 0;
    return prog_.newline && static_cast<unsigned char>(begin_[pos]) == '\n';
  }

  bool IsWordBeg(size_t pos) const {
    if (pos >= len_) return false;
    if (!IsWordByte(static_cast<unsigned char>(begin_[pos]))) return false;
    if (pos == 0) return !(begin_ > base_ && IsWordByte(static_cast<unsigned char>(begin_[-1])));
    return !IsWordByte(static_cast<unsigned char>(begin_[pos - 1]));
  }

  bool IsWordEnd(size_t pos) const {
    bool prev_word = (pos > 0) ? IsWordByte(static_cast<unsigned char>(begin_[pos - 1]))
                               : (begin_ > base_ && IsWordByte(static_cast<unsigned char>(begin_[-1])));
    if (!prev_word) return false;
    return pos >= len_ || !IsWordByte(static_cast<unsigned char>(begin_[pos]));
  }

  bool MatchConsumeOp(const Insn& insn, unsigned char ch) const {
    switch (insn.op) {
      case Op::kByte:
        return (prog_.icase ? FoldLower(ch) : ch) == insn.byte_val;
      case Op::kAny:
        return !prog_.newline || ch != '\n';
      case Op::kClass:
        return prog_.classes[insn.arg].Test(ch);
      default:
        return false;
    }
  }

  bool BetterThread(const Thread& a, const Thread& b) const {
    if (a.start != b.start) return a.start < b.start;
    for (size_t i = 2; i < num_slots_; i += 2) {
      int32_t aso = a.caps[i], bso = b.caps[i];
      if (aso != bso) {
        if (aso < 0) return false;
        if (bso < 0) return true;
        return aso < bso;
      }
      int32_t aeo = a.caps[i + 1], beo = b.caps[i + 1];
      if (aeo != beo) return aeo > beo;
    }
    return false;
  }

  int ExecuteNfa(std::vector<int32_t>& out_caps, int32_t& out_start, int32_t& out_end) {
    const size_t ninsns = prog_.insns.size();
    // Bound the two active sets, closure work list and their capture copies
    // before allocating. A resource limit reports REG_ESPACE, never NOMATCH.
    const size_t bytes_per_state = 8 * (sizeof(Thread) + num_slots_ * sizeof(int32_t)) + 16;
    if (ninsns > kMaxWorkBytes / bytes_per_state) return kRegEspace;
    std::vector<int32_t> slot_of(ninsns, -1);
    std::vector<Thread> curr;
    std::vector<Thread> next;

    bool has_match = false;
    int32_t best_start = 0;
    int32_t best_end = 0;
    std::vector<int32_t> best_caps;

    auto update_match = [&](const Thread& t, int32_t pos) {
      if (!has_match || t.start < best_start ||
          (t.start == best_start &&
           (pos > best_end || (pos == best_end && [&]() {
              for (size_t i = 2; i < num_slots_; i += 2) {
                if (t.caps[i] != best_caps[i]) {
                  if (t.caps[i] < 0) return false;
                  if (best_caps[i] < 0) return true;
                  return t.caps[i] < best_caps[i];
                }
                if (t.caps[i + 1] != best_caps[i + 1]) return t.caps[i + 1] > best_caps[i + 1];
              }
              return false;
            }())))) {
        has_match = true;
        best_start = t.start;
        best_end = pos;
        best_caps = t.caps;
      }
    };

    struct Frame {
      int32_t pc;
      std::vector<int32_t> caps;
    };
    std::vector<Frame> stack;
    std::vector<uint32_t> eps_vis(ninsns, 0);
    uint32_t eps_epoch = 0;

    auto add_thread_step = [&](std::vector<Thread>& dst, const Thread& seed, size_t pos) {
      stack.clear();
      stack.push_back(Frame{seed.pc, seed.caps});
      ++eps_epoch;
      if (eps_epoch == 0) {
        std::fill(eps_vis.begin(), eps_vis.end(), 0);
        ++eps_epoch;
      }

      while (!stack.empty()) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (f.pc < 0 || eps_vis[f.pc] == eps_epoch) continue;
        // Mark pure-control epsilon transitions so zero-width cycles `(a*)*` never loop.
        const Insn& insn = prog_.insns[f.pc];
        switch (insn.op) {
          case Op::kJump:
            eps_vis[f.pc] = eps_epoch;
            stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kSplit:
            eps_vis[f.pc] = eps_epoch;
            stack.push_back(Frame{insn.out2, f.caps});
            stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kBol:
            eps_vis[f.pc] = eps_epoch;
            if (IsBol(pos)) stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kEol:
            eps_vis[f.pc] = eps_epoch;
            if (IsEol(pos)) stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kWordBeg:
            eps_vis[f.pc] = eps_epoch;
            if (IsWordBeg(pos)) stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kWordEnd:
            eps_vis[f.pc] = eps_epoch;
            if (IsWordEnd(pos)) stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kSave:
            eps_vis[f.pc] = eps_epoch;
            if (static_cast<size_t>(insn.arg) < f.caps.size()) {
              f.caps[insn.arg] = static_cast<int32_t>(pos);
            }
            stack.push_back(Frame{insn.out1, std::move(f.caps)});
            break;
          case Op::kMatch: {
            Thread matched{f.pc, seed.start, std::move(f.caps)};
            update_match(matched, static_cast<int32_t>(pos));
            break;
          }
          case Op::kByte:
          case Op::kAny:
          case Op::kClass: {
            Thread cand{f.pc, seed.start, std::move(f.caps)};
            int32_t& existing = slot_of[f.pc];
            if (existing < 0) {
              existing = static_cast<int32_t>(dst.size());
              dst.push_back(std::move(cand));
            } else if (BetterThread(cand, dst[existing])) {
              dst[existing] = std::move(cand);
            }
            break;
          }
          case Op::kBackref:
            break;
        }
      }
    };

    const bool anchored_bol = (prog_.insns[prog_.entry].op == Op::kBol);
    for (size_t pos = 0; pos <= len_; ++pos) {
      if (!has_match && (!anchored_bol || IsBol(pos))) {
        Thread fresh;
        fresh.pc = prog_.entry;
        fresh.start = static_cast<int32_t>(pos);
        if (num_slots_ > 0) fresh.caps.assign(num_slots_, -1);
        add_thread_step(curr, fresh, pos);
      }

      if (curr.empty() && has_match) break;
      if (pos == len_) break;

      unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (const Thread& t : curr) slot_of[t.pc] = -1;
      for (Thread& t : curr) {
        if (has_match && t.start > best_start) continue;
        const Insn& insn = prog_.insns[t.pc];
        if (MatchConsumeOp(insn, ch)) {
          Thread adv{insn.out1, t.start, std::move(t.caps)};
          add_thread_step(next, adv, pos + 1);
        }
      }
      curr.swap(next);
      next.clear();
      // Keep next's PC slots valid after the swap so a new start merges with curr.
    }

    if (!has_match) return kRegNomatch;
    out_start = best_start;
    out_end = best_end;
    out_caps = std::move(best_caps);
    return kRegOk;
  }

  int ExecuteBackref(std::vector<int32_t>& out_caps, int32_t& out_start, int32_t& out_end) {
    const size_t slots_size = 2 * (prog_.nsub + 1);
    struct Frame {
      int32_t pc;
      size_t pos;
      std::vector<int32_t> caps;
      bool operator==(const Frame&) const = default;
    };
    struct FrameHash {
      size_t operator()(const Frame& f) const {
        size_t h = static_cast<uint32_t>(f.pc);
        auto mix = [&](size_t value) {
          h ^= value + size_t{0x9e3779b9} + (h << 6) + (h >> 2);
        };
        mix(f.pos);
        for (int32_t cap : f.caps) mix(static_cast<uint32_t>(cap));
        return h;
      }
    };
    // Backtracking uses heap-owned continuations, never one native stack frame
    // per input byte. Account conservatively for copies and hash-table overhead.
    const size_t state_bytes = 2 * (sizeof(Frame) + slots_size * sizeof(int32_t)) + 64;
    const size_t state_limit = kMaxWorkBytes / state_bytes;
    if (state_limit < 2) return kRegEspace;
    std::vector<Frame> pending;
    std::unordered_set<Frame, FrameHash> splits_seen;
    size_t steps = 0;
    bool has_match = false;
    int32_t best_start = 0;
    int32_t best_end = -1;
    std::vector<int32_t> best_caps;

    auto better_caps = [&](const std::vector<int32_t>& caps, int32_t end) {
      if (!has_match) return true;
      if (end != best_end) return end > best_end;
      for (size_t i = 2; i < slots_size; i += 2) {
        if (caps[i] != best_caps[i]) {
          if (caps[i] < 0) return false;
          if (best_caps[i] < 0) return true;
          return caps[i] < best_caps[i];
        }
        if (caps[i + 1] != best_caps[i + 1]) return caps[i + 1] > best_caps[i + 1];
      }
      return false;
    };

    for (size_t start = 0; start <= len_; ++start) {
      pending.clear();
      splits_seen.clear();
      pending.push_back(Frame{prog_.entry, start, std::vector<int32_t>(slots_size, -1)});
      while (!pending.empty()) {
        Frame current = std::move(pending.back());
        pending.pop_back();
        while (current.pc >= 0) {
          if (++steps > kMaxBackrefSteps) return kRegEspace;
          const Insn& insn = prog_.insns[current.pc];
          switch (insn.op) {
            case Op::kMatch:
              if (better_caps(current.caps, static_cast<int32_t>(current.pos))) {
                has_match = true;
                best_start = static_cast<int32_t>(start);
                best_end = static_cast<int32_t>(current.pos);
                best_caps = current.caps;
              }
              current.pc = -1;
              break;
            case Op::kJump:
              current.pc = insn.out1;
              break;
            case Op::kSplit:
              // Every compiled cycle passes through a split. The whole capture
              // state is part of the key, so an empty repetition terminates
              // without discarding a distinct backreference/capture outcome.
              if (splits_seen.find(current) != splits_seen.end()) {
                current.pc = -1;
                break;
              }
              if (pending.size() + splits_seen.size() + 2 > state_limit) return kRegEspace;
              splits_seen.insert(current);
              pending.push_back(Frame{insn.out2, current.pos, current.caps});
              current.pc = insn.out1;
              break;
            case Op::kBol:
              current.pc = IsBol(current.pos) ? insn.out1 : -1;
              break;
            case Op::kEol:
              current.pc = IsEol(current.pos) ? insn.out1 : -1;
              break;
            case Op::kWordBeg:
              current.pc = IsWordBeg(current.pos) ? insn.out1 : -1;
              break;
            case Op::kWordEnd:
              current.pc = IsWordEnd(current.pos) ? insn.out1 : -1;
              break;
            case Op::kSave:
              current.caps[insn.arg] = static_cast<int32_t>(current.pos);
              current.pc = insn.out1;
              break;
            case Op::kByte:
            case Op::kAny:
            case Op::kClass:
              if (current.pos < len_ &&
                  MatchConsumeOp(insn, static_cast<unsigned char>(begin_[current.pos]))) {
                ++current.pos;
                current.pc = insn.out1;
              } else {
                current.pc = -1;
              }
              break;
            case Op::kBackref: {
              const size_t idx = static_cast<size_t>(insn.arg);
              const int32_t rso = current.caps[2 * idx];
              const int32_t reo = current.caps[2 * idx + 1];
              if (rso < 0 || reo < rso) {
                current.pc = -1;
                break;
              }
              const size_t length = static_cast<size_t>(reo - rso);
              bool matches = length <= len_ - current.pos;
              for (size_t i = 0; matches && i < length; ++i) {
                if (++steps > kMaxBackrefSteps) return kRegEspace;
                const auto expected = static_cast<unsigned char>(begin_[rso + i]);
                const auto actual = static_cast<unsigned char>(begin_[current.pos + i]);
                // AOSP's OBACK_ compares the captured bytes with memcmp even
                // when REG_ICASE was used to compile the capturing expression.
                matches = expected == actual;
              }
              if (matches) {
                current.pos += length;
                current.pc = insn.out1;
              } else {
                current.pc = -1;
              }
              break;
            }
          }
        }
      }
      if (has_match) break;
    }
    if (!has_match) return kRegNomatch;
    out_start = best_start;
    out_end = best_end;
    if (track_caps_) out_caps = std::move(best_caps);
    return kRegOk;
  }

  const Program& prog_;
  const char* base_;
  const char* begin_;
  size_t len_;
  int eflags_;
  bool track_caps_;
  size_t num_slots_;
};

}  // namespace gesher::bionic::posix_regex
