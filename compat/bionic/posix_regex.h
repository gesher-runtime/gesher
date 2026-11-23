// POSIX/Bionic-compatible regular expression engine for compat/bionic.
// Supports BRE (REG_BASIC) and ERE (REG_EXTENDED), REG_ICASE, REG_NOSUB,
// REG_NEWLINE (NetBSD/Android per-byte LF anchor/negated-class semantics),
// REG_PEND, REG_NOTBOL, REG_NOTEOL, and REG_STARTEND, with linear-time
// leftmost-longest whole-match NFA execution and AOSP NetBSD-compatible
// subexpression dissection / backreference backtracking.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
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
constexpr size_t kMaxAstNodes = 32768;
constexpr size_t kMaxBackrefSteps = 2000000;
constexpr size_t kMaxDissectSteps = 50000000;
constexpr size_t kMaxEmptyBackrefRecursion = 100;
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
  kPlusBeg,
  kPlusEnd,
  kSplit,
  kAltSplit,
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

enum class AstKind : uint8_t {
  kEmpty,
  kLeaf,
  kGroup,
  kConcat,
  kAlt,
  kPlus,
  kQuest,
};

struct AstNode {
  AstKind kind = AstKind::kEmpty;
  int32_t arg = 0;
  int32_t entry_pc = -1;
  int32_t exit_pc = -1;
  std::vector<int32_t> children;
};

struct Program {
  std::vector<Insn> insns;
  std::vector<AstNode> nodes;
  std::vector<CharClass> classes;
  int32_t entry = -1;
  int32_t root_ast = -1;
  int32_t accept_pc = -1;
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
  Compiler(std::string pat, bool extended, bool icase, bool nosub, bool newline)
      : pat_(std::move(pat)), extended_(extended) {
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
    int32_t match_pc = Emit(Op::kMatch);
    if (match_pc < 0) return kRegEspace;
    prog_.insns[root.exit_pc].out1 = match_pc;
    prog_.entry = root.entry_pc;
    prog_.root_ast = root.ast_id;
    prog_.accept_pc = match_pc;
    if (prog_.insns.size() > kMaxInsns || prog_.nodes.size() > kMaxAstNodes) return kRegEspace;
    out_prog = std::move(prog_);
    return kRegOk;
  }

 private:
  struct Frag {
    int32_t ast_id = -1;
    int32_t entry_pc = -1;
    int32_t exit_pc = -1;
    bool repeatable = false;
    bool elided = false;
  };

  int32_t Emit(Op op, uint8_t b = 0, int32_t arg = 0, int32_t out1 = -1, int32_t out2 = -1) {
    if (prog_.insns.size() >= kMaxInsns) return -1;
    int32_t pc = static_cast<int32_t>(prog_.insns.size());
    prog_.insns.push_back(Insn{op, b, arg, out1, out2});
    return pc;
  }

  int32_t AddAst(AstKind kind, int32_t arg, int32_t entry_pc, int32_t exit_pc,
                 std::vector<int32_t> children = {}) {
    if (prog_.nodes.size() >= kMaxAstNodes) return -1;
    int32_t id = static_cast<int32_t>(prog_.nodes.size());
    prog_.nodes.push_back(AstNode{kind, arg, entry_pc, exit_pc, std::move(children)});
    return id;
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

    return BuildAltFrag(branches, /*repeatable=*/true, out);
  }

  int BuildAltFrag(const std::vector<Frag>& branches, bool repeatable, Frag& out) {
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    std::vector<int32_t> child_ids;
    child_ids.reserve(branches.size());
    for (const Frag& b : branches) {
      prog_.insns[b.exit_pc].out1 = exit_pc;
      child_ids.push_back(b.ast_id);
    }
    int32_t tail_pc = branches.back().entry_pc;
    for (size_t i = branches.size() - 1; i > 0; --i) {
      int32_t sp = Emit(Op::kAltSplit, 0, exit_pc, branches[i - 1].entry_pc, tail_pc);
      if (sp < 0) return kRegEspace;
      tail_pc = sp;
    }
    int32_t ast_id = AddAst(AstKind::kAlt, 0, tail_pc, exit_pc, std::move(child_ids));
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, tail_pc, exit_pc, repeatable, false};
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

  int MakeEmptyFrag(Frag& out, bool elided = false) {
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    int32_t ast_id = AddAst(AstKind::kEmpty, 0, exit_pc, exit_pc);
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, exit_pc, exit_pc, false, elided};
    return kRegOk;
  }

  int MakeLeafFrag(Op op, uint8_t b, int32_t arg, bool repeatable, Frag& out) {
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    int32_t pc = Emit(op, b, arg, exit_pc, -1);
    if (pc < 0) return kRegEspace;
    int32_t ast_id = AddAst(AstKind::kLeaf, pc, pc, exit_pc);
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, pc, exit_pc, repeatable, false};
    return kRegOk;
  }

  // Android's OBACK_ encloses a copy of the referenced group's language. Its
  // whole-pattern NFA follows that copy before backref() checks exact bytes.
  // Keep the relaxed language on out2; the byte-equality executor uses out1.
  int CopyBackrefLanguage(size_t gid, int32_t continuation, int32_t& entry) {
    const auto [body_entry, body_exit] = group_bodies_[gid];
    const size_t original_size = prog_.insns.size();
    std::vector<int32_t> copies(original_size, -1);
    std::vector<int32_t> pending;
    auto copy_pc = [&](int32_t pc) -> int32_t {
      if (pc < 0) return -1;
      if (copies[pc] >= 0) return copies[pc];
      int32_t copy = Emit(Op::kJump);
      if (copy >= 0) {
        copies[pc] = copy;
        pending.push_back(pc);
      }
      return copy;
    };
    entry = copy_pc(body_entry);
    if (entry < 0) return kRegEspace;
    while (!pending.empty()) {
      int32_t pc = pending.back();
      pending.pop_back();
      const int32_t copy = copies[pc];
      if (pc == body_exit) {
        prog_.insns[copy] = Insn{Op::kJump, 0, 0, continuation, -1};
        continue;
      }
      Insn insn = prog_.insns[pc];
      if (insn.op == Op::kBackref) {
        insn.op = Op::kJump;
        insn.out1 = insn.out2;
        insn.out2 = -1;
      } else if (insn.op == Op::kAltSplit || insn.op == Op::kPlusEnd) {
        insn.op = Op::kSplit;
      } else if (insn.op == Op::kSave || insn.op == Op::kPlusBeg) {
        insn.op = Op::kJump;
      }
      for (int32_t* target : {&insn.out1, &insn.out2}) {
        if (*target >= 0) {
          *target = copy_pc(*target);
          if (*target < 0) return kRegEspace;
        }
      }
      prog_.insns[copy] = insn;
    }
    return kRegOk;
  }

  int BuildConcatFrag(std::vector<Frag>& items, bool repeatable, Frag& out) {
    if (items.size() == 1) {
      out = std::move(items[0]);
      out.repeatable = repeatable;
      return kRegOk;
    }
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    std::vector<int32_t> child_ids;
    child_ids.reserve(items.size());
    bool all_elided = true;
    for (size_t i = 0; i < items.size(); ++i) {
      all_elided = all_elided && items[i].elided;
      child_ids.push_back(items[i].ast_id);
      int32_t next_entry = (i + 1 < items.size()) ? items[i + 1].entry_pc : exit_pc;
      prog_.insns[items[i].exit_pc].out1 = next_entry;
    }
    int32_t entry_pc = items.front().entry_pc;
    int32_t ast_id = AddAst(AstKind::kConcat, 0, entry_pc, exit_pc, std::move(child_ids));
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, entry_pc, exit_pc, repeatable, all_elided};
    return kRegOk;
  }

  int ParseBranch(Frag& out, bool stop_on_rparen) {
    bool at_piece_start = true;
    std::vector<Frag> pieces;
    bool all_elided = true;
    while (!AtStop(stop_on_rparen)) {
      Frag piece;
      int err = ParsePiece(piece, at_piece_start, stop_on_rparen);
      if (err != kRegOk) return err;
      at_piece_start = false;
      all_elided = all_elided && piece.elided;
      pieces.push_back(std::move(piece));
    }
    // NetBSD permits an explicitly empty group, but rejects an empty expression
    // or alternative, including one erased entirely by a {0} repetition.
    if (pieces.empty() || all_elided) return kRegEmpty;
    int err = BuildConcatFrag(pieces, /*repeatable=*/false, out);
    if (err == kRegOk) out.elided = false;
    return err;
  }

  int ParseAtomSlice(size_t start_pos, size_t end_pos, bool at_piece_start, size_t base_nsub,
                     size_t max_nsub, Frag& out) {
    size_t saved_pos = pos_;
    const auto saved_closed = closed_groups_;
    const auto saved_bodies = group_bodies_;
    pos_ = start_pos;
    prog_.nsub = base_nsub;
    int err = ParseAtom(out, at_piece_start, /*stop_on_rparen=*/true);
    if (err == kRegOk && pos_ != end_pos) err = kRegBadpat;
    pos_ = saved_pos;
    closed_groups_ = saved_closed;
    group_bodies_ = saved_bodies;
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
      if (pos_ < pat_.size()) {
        char ch = pat_[pos_];
        if (ch == '*' || ch == '+' || ch == '?') {
          if (!atom.repeatable) return kRegBadrpt;
          ++pos_;
          if (pos_ < pat_.size() &&
              (pat_[pos_] == '*' || pat_[pos_] == '+' || pat_[pos_] == '?' || pat_[pos_] == '{')) {
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
          if (pos_ < pat_.size() &&
              (pat_[pos_] == '*' || pat_[pos_] == '+' || pat_[pos_] == '?' || pat_[pos_] == '{')) {
            return kRegBadrpt;
          }
          return ApplyBoundedRep(atom, atom_start, atom_end, at_piece_start, base_nsub, max_nsub,
                                 min_rep, max_rep, unbounded, out);
        }
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

  int BuildPlusFrag(const Frag& body, Frag& out) {
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    int32_t beg_pc = Emit(Op::kPlusBeg, 0, 0, body.entry_pc, -1);
    int32_t end_pc = Emit(Op::kPlusEnd, 0, 0, body.entry_pc, exit_pc);
    if (beg_pc < 0 || end_pc < 0) return kRegEspace;
    prog_.insns[body.exit_pc].out1 = end_pc;
    int32_t ast_id = AddAst(AstKind::kPlus, 0, beg_pc, exit_pc, {body.ast_id});
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, beg_pc, exit_pc, false, false};
    return kRegOk;
  }

  int BuildQuestFrag(const Frag& body, Frag& out) {
    int32_t exit_pc = Emit(Op::kJump);
    if (exit_pc < 0) return kRegEspace;
    int32_t sp = Emit(Op::kSplit, 0, 0, body.entry_pc, exit_pc);
    if (sp < 0) return kRegEspace;
    prog_.insns[body.exit_pc].out1 = exit_pc;
    int32_t ast_id = AddAst(AstKind::kQuest, 0, sp, exit_pc, {body.ast_id});
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, sp, exit_pc, false, false};
    return kRegOk;
  }

  int BuildOptionalChoiceFrag(const Frag& body, Frag& out) {
    // Matches NetBSD regcomp.c: A? and {0,1} emit (A|empty).
    Frag empty_branch;
    int err = MakeEmptyFrag(empty_branch);
    if (err != kRegOk) return err;
    std::vector<Frag> branches{body, empty_branch};
    return BuildAltFrag(branches, /*repeatable=*/false, out);
  }

  int ApplySimpleRep(Frag& atom, char op, Frag& out) {
    if (op == '*') {
      // Matches NetBSD regcomp.c: '*' is OQUEST_(OPLUS_(atom)).
      Frag plus;
      int err = BuildPlusFrag(atom, plus);
      if (err != kRegOk) return err;
      return BuildQuestFrag(plus, out);
    }
    if (op == '+') {
      return BuildPlusFrag(atom, out);
    }
    return BuildOptionalChoiceFrag(atom, out);
  }

  // Expands {min, max} following NetBSD regcomp.c's repeat() decomposition so
  // nested and optional capture selections match Android 16 Bionic verbatim.
  template <typename NextCopyFn>
  int ExpandNetBsdRepeat(size_t min_rep, size_t max_rep, bool unbounded,
                         NextCopyFn& next_copy, Frag& out) {
    if (min_rep == 0 && !unbounded && max_rep == 0) {
      return MakeEmptyFrag(out, /*elided=*/true);
    }
    if (min_rep == 0) {
      Frag inner;
      int err = ExpandNetBsdRepeat(1, max_rep, unbounded, next_copy, inner);
      if (err != kRegOk) return err;
      return BuildOptionalChoiceFrag(inner, out);
    }
    if (min_rep == 1 && !unbounded && max_rep == 1) {
      return next_copy(out);
    }
    if (min_rep == 1 && unbounded) {
      Frag single;
      int err = next_copy(single);
      if (err != kRegOk) return err;
      return BuildPlusFrag(single, out);
    }
    if (min_rep == 1) {
      Frag single;
      int err = next_copy(single);
      if (err != kRegOk) return err;
      Frag head;
      err = BuildOptionalChoiceFrag(single, head);
      if (err != kRegOk) return err;
      Frag tail;
      err = ExpandNetBsdRepeat(1, max_rep - 1, false, next_copy, tail);
      if (err != kRegOk) return err;
      std::vector<Frag> items{head, tail};
      return BuildConcatFrag(items, /*repeatable=*/false, out);
    }
    Frag head;
    int err = next_copy(head);
    if (err != kRegOk) return err;
    Frag tail;
    err = ExpandNetBsdRepeat(min_rep - 1, unbounded ? 0 : (max_rep - 1), unbounded, next_copy, tail);
    if (err != kRegOk) return err;
    std::vector<Frag> items{head, tail};
    return BuildConcatFrag(items, /*repeatable=*/false, out);
  }

  int ApplyBoundedRep(Frag& first_atom, size_t atom_start, size_t atom_end, bool at_piece_start,
                      size_t base_nsub, size_t max_nsub, size_t min_rep, size_t max_rep,
                      bool unbounded, Frag& out) {
    bool used_first = false;
    auto next_copy = [&](Frag& dst) -> int {
      if (!used_first) {
        used_first = true;
        dst = std::move(first_atom);
        return kRegOk;
      }
      return ParseAtomSlice(atom_start, atom_end, at_piece_start, base_nsub, max_nsub, dst);
    };
    return ExpandNetBsdRepeat(min_rep, max_rep, unbounded, next_copy, out);
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
        return MakeLeafFrag(Op::kBol, 0, 0, /*repeatable=*/false, out);
      }
      if (ch == '$') {
        ++pos_;
        return MakeLeafFrag(Op::kEol, 0, 0, /*repeatable=*/false, out);
      }
    } else {
      if (ch == '^' && at_piece_start) {
        ++pos_;
        return MakeLeafFrag(Op::kBol, 0, 0, /*repeatable=*/false, out);
      }
      if (ch == '$' && (pos_ + 1 == pat_.size() ||
                        (stop_on_rparen && pos_ + 2 < pat_.size() &&
                         pat_[pos_ + 1] == '\\' && pat_[pos_ + 2] == ')'))) {
        ++pos_;
        return MakeLeafFrag(Op::kEol, 0, 0, /*repeatable=*/false, out);
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
          int err = MakeLeafFrag(Op::kBackref, 0, static_cast<int32_t>(idx), /*repeatable=*/true, out);
          if (err != kRegOk) return err;
          int32_t language_entry = -1;
          err = CopyBackrefLanguage(idx, out.exit_pc, language_entry);
          if (err == kRegOk) prog_.insns[out.entry_pc].out2 = language_entry;
          return err;
        }
      }
    }

    if (ch == '.') {
      ++pos_;
      return MakeLeafFrag(Op::kAny, 0, 0, /*repeatable=*/true, out);
    }
    if (ch == '[') {
      return ParseBracket(out);
    }
    if (ch == '\\') {
      if (pos_ + 1 >= pat_.size()) return kRegEescape;
      char next = pat_[pos_ + 1];
      if (next == '<') {
        pos_ += 2;
        return MakeLeafFrag(Op::kWordBeg, 0, 0, /*repeatable=*/false, out);
      }
      if (next == '>') {
        pos_ += 2;
        return MakeLeafFrag(Op::kWordEnd, 0, 0, /*repeatable=*/false, out);
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

    Frag inner;
    const bool empty_group = basic_group
                                 ? pos_ + 1 < pat_.size() && pat_[pos_] == '\\' &&
                                       pat_[pos_ + 1] == ')'
                                 : pos_ < pat_.size() && pat_[pos_] == ')';
    if (empty_group) {
      int err = MakeEmptyFrag(inner);
      if (err != kRegOk) return err;
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

    if (gid < closed_groups_.size()) {
      closed_groups_[gid] = true;
      group_bodies_[gid] = {inner.entry_pc, inner.exit_pc};
    }
    int32_t exit_pc = Emit(Op::kJump);
    int32_t open_pc = Emit(Op::kSave, 0, static_cast<int32_t>(2 * gid), inner.entry_pc, -1);
    int32_t close_pc = Emit(Op::kSave, 0, static_cast<int32_t>(2 * gid + 1), exit_pc, -1);
    if (exit_pc < 0 || open_pc < 0 || close_pc < 0) return kRegEspace;
    prog_.insns[inner.exit_pc].out1 = close_pc;
    int32_t ast_id = AddAst(AstKind::kGroup, static_cast<int32_t>(gid), open_pc, exit_pc,
                            {inner.ast_id});
    if (ast_id < 0) return kRegEspace;
    out = Frag{ast_id, open_pc, exit_pc, /*repeatable=*/true, false};
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
      return MakeLeafFrag(op, 0, 0, /*repeatable=*/true, out);
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
    return MakeLeafFrag(Op::kClass, 0, cid, /*repeatable=*/true, out);
  }

  int MakeByte(uint8_t b, Frag& out) {
    uint8_t stored = prog_.icase ? FoldLower(b) : b;
    return MakeLeafFrag(Op::kByte, stored, 0, /*repeatable=*/true, out);
  }

  std::string pat_;
  bool extended_;
  size_t pos_ = 0;
  std::array<bool, 10> closed_groups_{};
  std::array<std::pair<int32_t, int32_t>, 10> group_bodies_{};
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
        need_match_span_(!prog.nosub && nmatch != 0),
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
    int32_t tag = 0;  // start offset in ExecuteNfa, or max origin offset in FindMaxConcatSplit.
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
    if (pos == 0) {
      if ((eflags_ & kRegNotbol) == 0) return true;
      return begin_ > base_ && !IsWordByte(static_cast<unsigned char>(begin_[-1]));
    }
    return !IsWordByte(static_cast<unsigned char>(begin_[pos - 1]));
  }

  bool IsWordEnd(size_t pos) const {
    if (pos == 0) return false;
    if (!IsWordByte(static_cast<unsigned char>(begin_[pos - 1]))) return false;
    if (pos == len_) return (eflags_ & kRegNoteol) == 0;
    return !IsWordByte(static_cast<unsigned char>(begin_[pos]));
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

  static constexpr size_t kNoMatch = std::numeric_limits<size_t>::max();

  struct DissectScratch {
    std::vector<int32_t> slot_of;
    std::vector<uint32_t> eps_vis;
    uint32_t eps_epoch = 0;
    std::vector<int32_t> stack;
    std::vector<Thread> curr;
    std::vector<Thread> next;
    std::unique_ptr<uint8_t[]> prefix_ok;
    size_t prefix_capacity = 0;
    size_t prefix_budget;
    size_t work_steps = 0;

    DissectScratch(size_t ninsns, size_t available_prefix_bytes)
        : slot_of(ninsns, -1), eps_vis(ninsns, 0), prefix_budget(available_prefix_bytes) {}

    uint32_t NextEpoch() {
      ++eps_epoch;
      if (eps_epoch == 0) {
        std::fill(eps_vis.begin(), eps_vis.end(), 0);
        ++eps_epoch;
      }
      return eps_epoch;
    }
  };

  template <typename OnAccept>
  bool AddSubStep(DissectScratch& sc, std::vector<Thread>& dst, int32_t seed_pc, int32_t seed_tag,
                  int32_t stop_pc, size_t pos, bool keep_max_tag, OnAccept&& on_accept) const {
    sc.stack.clear();
    sc.stack.push_back(seed_pc);
    const uint32_t epoch = sc.NextEpoch();
    while (!sc.stack.empty()) {
      if (++sc.work_steps > kMaxDissectSteps) return false;
      int32_t pc = sc.stack.back();
      sc.stack.pop_back();
      if (pc < 0 || sc.eps_vis[pc] == epoch) continue;
      sc.eps_vis[pc] = epoch;
      if (pc == stop_pc) {
        on_accept(seed_tag);
        continue;
      }
      const Insn& insn = prog_.insns[pc];
      switch (insn.op) {
        case Op::kJump:
        case Op::kSave:
        case Op::kPlusBeg:
          sc.stack.push_back(insn.out1);
          break;
        case Op::kSplit:
        case Op::kAltSplit:
        case Op::kPlusEnd:
          sc.stack.push_back(insn.out2);
          sc.stack.push_back(insn.out1);
          break;
        case Op::kBol:
          if (IsBol(pos)) sc.stack.push_back(insn.out1);
          break;
        case Op::kEol:
          if (IsEol(pos)) sc.stack.push_back(insn.out1);
          break;
        case Op::kWordBeg:
          if (IsWordBeg(pos)) sc.stack.push_back(insn.out1);
          break;
        case Op::kWordEnd:
          if (IsWordEnd(pos)) sc.stack.push_back(insn.out1);
          break;
        case Op::kMatch:
          break;
        case Op::kByte:
        case Op::kAny:
        case Op::kClass: {
          int32_t& existing = sc.slot_of[pc];
          if (existing < 0) {
            existing = static_cast<int32_t>(dst.size());
            dst.push_back(Thread{pc, seed_tag});
          } else if (keep_max_tag ? (seed_tag > dst[existing].tag) : (seed_tag < dst[existing].tag)) {
            dst[existing].tag = seed_tag;
          }
          break;
        }
        case Op::kBackref:
          break;
      }
    }
    return true;
  }

  // Runs sub-NFA `[entry_pc .. stop_pc)` across `pos in [sp, limit]`.
  // Returns `kNoMatch` (or sets `timed_out = true` on step budget) if unmatched.
  size_t WalkSubMatch(DissectScratch& sc, int32_t entry_pc, int32_t stop_pc, size_t sp,
                      size_t limit, bool require_full, bool& timed_out) const {
    sc.curr.clear();
    sc.next.clear();
    size_t best_end = kNoMatch;
    if (!AddSubStep(sc, sc.curr, entry_pc, 0, stop_pc, sp, /*keep_max_tag=*/false, [&](int32_t) {
          if (!require_full || sp == limit) best_end = sp;
        })) {
      timed_out = true;
      return kNoMatch;
    }
    for (size_t pos = sp; pos < limit; ++pos) {
      if (sc.curr.empty()) break;
      const unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;
      for (const Thread& t : sc.curr) {
        const Insn& insn = prog_.insns[t.pc];
        if (MatchConsumeOp(insn, ch)) {
          if (!AddSubStep(sc, sc.next, insn.out1, 0, stop_pc, pos + 1, /*keep_max_tag=*/false,
                          [&](int32_t) {
                            if (!require_full || pos + 1 == limit) best_end = pos + 1;
                          })) {
            for (const Thread& nt : sc.next) sc.slot_of[nt.pc] = -1;
            timed_out = true;
            return kNoMatch;
          }
        }
      }
      sc.curr.swap(sc.next);
      sc.next.clear();
      if (require_full && best_end == limit && pos + 1 == limit) break;
    }
    for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;
    return best_end;
  }

  // Finds largest `r in [sp, stop]` where prefix `[prefix_entry .. prefix_exit)` matches `[sp, r]`
  // and suffix `[suffix_entry .. suffix_exit)` matches `[r, stop]`.
  size_t FindMaxConcatSplit(DissectScratch& sc, int32_t prefix_node_id, int32_t suffix_entry_pc,
                            int32_t suffix_exit_pc, size_t sp, size_t stop, bool& timed_out) const {
    const AstNode& pnode = prog_.nodes[prefix_node_id];
    if (pnode.kind == AstKind::kEmpty) return sp;
    if (pnode.kind == AstKind::kLeaf) {
      const Op op = prog_.insns[pnode.arg].op;
      if (op == Op::kBol || op == Op::kEol || op == Op::kWordBeg || op == Op::kWordEnd) return sp;
      if (op == Op::kByte || op == Op::kAny || op == Op::kClass) return sp + 1;
    }

    const size_t span = stop - sp;
    if (span >= sc.prefix_budget) {
      timed_out = true;
      return kNoMatch;
    }
    if (span + 1 > sc.prefix_capacity) {
      sc.prefix_ok.reset();
      sc.prefix_capacity = 0;
      sc.prefix_ok = std::make_unique<uint8_t[]>(span + 1);
      sc.prefix_capacity = span + 1;
    } else {
      std::fill_n(sc.prefix_ok.get(), span + 1, uint8_t{0});
    }
    sc.curr.clear();
    sc.next.clear();
    if (!AddSubStep(sc, sc.curr, pnode.entry_pc, 0, pnode.exit_pc, sp, /*keep_max_tag=*/false,
                    [&](int32_t) { sc.prefix_ok[0] = 1; })) {
      timed_out = true;
      return kNoMatch;
    }
    for (size_t pos = sp; pos < stop; ++pos) {
      if (sc.curr.empty()) break;
      const unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;
      for (const Thread& t : sc.curr) {
        const Insn& insn = prog_.insns[t.pc];
        if (MatchConsumeOp(insn, ch)) {
          if (!AddSubStep(sc, sc.next, insn.out1, 0, pnode.exit_pc, pos + 1,
                          /*keep_max_tag=*/false,
                          [&](int32_t) { sc.prefix_ok[pos + 1 - sp] = 1; })) {
            for (const Thread& nt : sc.next) sc.slot_of[nt.pc] = -1;
            timed_out = true;
            return kNoMatch;
          }
        }
      }
      sc.curr.swap(sc.next);
      sc.next.clear();
    }
    for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;

    sc.curr.clear();
    sc.next.clear();
    int32_t best_split = -1;
    for (size_t pos = sp; pos <= stop; ++pos) {
      if (sc.prefix_ok[pos - sp]) {
        if (!AddSubStep(sc, sc.curr, suffix_entry_pc, static_cast<int32_t>(pos), suffix_exit_pc,
                        pos, /*keep_max_tag=*/true, [&](int32_t tag) {
                          if (pos == stop && tag > best_split) best_split = tag;
                        })) {
          for (const Thread& ct : sc.curr) sc.slot_of[ct.pc] = -1;
          timed_out = true;
          return kNoMatch;
        }
      }
      if (pos == stop) break;
      const unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;
      for (const Thread& t : sc.curr) {
        const Insn& insn = prog_.insns[t.pc];
        if (MatchConsumeOp(insn, ch)) {
          if (!AddSubStep(sc, sc.next, insn.out1, t.tag, suffix_exit_pc, pos + 1,
                          /*keep_max_tag=*/true, [&](int32_t tag) {
                            if (pos + 1 == stop && tag > best_split) best_split = tag;
                          })) {
            for (const Thread& nt : sc.next) sc.slot_of[nt.pc] = -1;
            timed_out = true;
            return kNoMatch;
          }
        }
      }
      sc.curr.swap(sc.next);
      sc.next.clear();
    }
    for (const Thread& t : sc.curr) sc.slot_of[t.pc] = -1;
    return (best_split >= 0) ? static_cast<size_t>(best_split) : kNoMatch;
  }

  struct DissectTask {
    int32_t node_id;
    size_t start;
    size_t end;
  };

  // Preserve left-to-right captures without recursion over generated concat
  // nodes, whose depth can exceed the parsed group nesting depth.
  int Dissect(DissectScratch& sc, int32_t root, size_t start, size_t end,
              std::vector<int32_t>& caps) const {
    std::vector<DissectTask> tasks;
    tasks.reserve(prog_.nodes.size());
    tasks.push_back({root, start, end});
    while (!tasks.empty()) {
      const auto [node_id, sp, stop] = tasks.back();
      tasks.pop_back();
      const AstNode& node = prog_.nodes[node_id];
      switch (node.kind) {
      case AstKind::kEmpty:
      case AstKind::kLeaf:
        break;
      case AstKind::kGroup: {
        const size_t gid = static_cast<size_t>(node.arg);
        if (2 * gid + 1 < caps.size()) {
          caps[2 * gid] = static_cast<int32_t>(sp);
          caps[2 * gid + 1] = static_cast<int32_t>(stop);
        }
        tasks.push_back({node.children[0], sp, stop});
        break;
      }
      case AstKind::kConcat: {
        size_t cur = sp;
        const size_t first_task = tasks.size();
        for (size_t i = 0; i < node.children.size(); ++i) {
          size_t part_end = stop;
          if (i + 1 < node.children.size()) {
            bool timed_out = false;
            const AstNode& next_node = prog_.nodes[node.children[i + 1]];
            part_end = FindMaxConcatSplit(sc, node.children[i], next_node.entry_pc, node.exit_pc,
                                          cur, stop, timed_out);
            if (timed_out) return kRegEspace;
            if (part_end == kNoMatch) break;
          }
          tasks.push_back({node.children[i], cur, part_end});
          cur = part_end;
        }
        std::reverse(tasks.begin() + first_task, tasks.end());
        break;
      }
      case AstKind::kAlt: {
        for (int32_t branch_id : node.children) {
          const AstNode& b = prog_.nodes[branch_id];
          bool timed_out = false;
          if (WalkSubMatch(sc, b.entry_pc, b.exit_pc, sp, stop, /*require_full=*/true, timed_out) !=
              kNoMatch) {
            tasks.push_back({branch_id, sp, stop});
            break;
          }
          if (timed_out) return kRegEspace;
        }
        break;
      }
      case AstKind::kQuest: {
        const AstNode& child = prog_.nodes[node.children[0]];
        bool timed_out = false;
        if (WalkSubMatch(sc, child.entry_pc, child.exit_pc, sp, stop, /*require_full=*/true,
                         timed_out) != kNoMatch) {
          tasks.push_back({node.children[0], sp, stop});
        }
        if (timed_out) return kRegEspace;
        break;
      }
      case AstKind::kPlus: {
        const AstNode& child = prog_.nodes[node.children[0]];
        size_t ssp = sp;
        size_t oldssp = ssp;
        size_t sep = sp;
        for (;;) {
          bool timed_out = false;
          sep = WalkSubMatch(sc, child.entry_pc, child.exit_pc, ssp, stop, /*require_full=*/false,
                             timed_out);
          if (timed_out) return kRegEspace;
          if (sep == kNoMatch || sep == ssp) break;
          oldssp = ssp;
          ssp = sep;
        }
        if (sep == kNoMatch) {
          sep = ssp;
          ssp = oldssp;
        }
        tasks.push_back({node.children[0], ssp, sep});
        break;
      }
      }
    }
    return kRegOk;
  }

  int ExecuteNfa(std::vector<int32_t>& out_caps, int32_t& out_start, int32_t& out_end) {
    const size_t ninsns = prog_.insns.size();
    const size_t bytes_per_state = 8 * sizeof(Thread) + 16;
    // Keep the original capacity headroom and account for the best-start tag
    // added to each epsilon visit. Capture dissection still uses its own scratch.
    const size_t nfa_bytes_per_state = bytes_per_state + sizeof(int32_t);
    if (ninsns > kMaxWorkBytes / nfa_bytes_per_state) return kRegEspace;
    std::vector<int32_t> slot_of(ninsns, -1);
    std::vector<Thread> curr;
    std::vector<Thread> next;

    bool has_match = false;
    int32_t best_start = 0;
    int32_t best_end = 0;

    auto update_match = [&](int32_t start, int32_t pos) {
      if (!has_match || start < best_start || (start == best_start && pos > best_end)) {
        has_match = true;
        best_start = start;
        best_end = pos;
      }
    };

    std::vector<int32_t> stack;
    struct Visit {
      uint32_t position = 0;
      int32_t start = 0;
    };
    std::vector<Visit> eps_vis(ninsns);

    auto add_thread_step = [&](std::vector<Thread>& dst, int32_t seed_pc, int32_t seed_start,
                               size_t pos) {
      const uint32_t position = static_cast<uint32_t>(pos) + 1;
      stack.clear();
      stack.push_back(seed_pc);

      while (!stack.empty()) {
        int32_t pc = stack.back();
        stack.pop_back();
        if (pc < 0) continue;
        Visit& visit = eps_vis[pc];
        // At the same input position, this phase has no capture history: only
        // the earliest start can win. Retain that traversal across all seeds,
        // including a newly injected start. A better start must propagate even
        // if a later-starting thread reached this state first. Execute() bounds
        // positions to INT32_MAX, so position+1 cannot wrap to the unvisited
        // zero marker. This also lets the scratch start completely zeroed.
        if (visit.position == position && visit.start <= seed_start) continue;
        visit = {position, seed_start};
        const Insn& insn = prog_.insns[pc];
        switch (insn.op) {
          case Op::kJump:
          case Op::kSave:
          case Op::kPlusBeg:
            stack.push_back(insn.out1);
            break;
          case Op::kSplit:
          case Op::kAltSplit:
          case Op::kPlusEnd:
            stack.push_back(insn.out2);
            stack.push_back(insn.out1);
            break;
          case Op::kBol:
            if (IsBol(pos)) stack.push_back(insn.out1);
            break;
          case Op::kEol:
            if (IsEol(pos)) stack.push_back(insn.out1);
            break;
          case Op::kWordBeg:
            if (IsWordBeg(pos)) stack.push_back(insn.out1);
            break;
          case Op::kWordEnd:
            if (IsWordEnd(pos)) stack.push_back(insn.out1);
            break;
          case Op::kMatch:
            update_match(seed_start, static_cast<int32_t>(pos));
            break;
          case Op::kByte:
          case Op::kAny:
          case Op::kClass: {
            int32_t& existing = slot_of[pc];
            if (existing < 0) {
              existing = static_cast<int32_t>(dst.size());
              dst.push_back(Thread{pc, seed_start});
            } else if (seed_start < dst[existing].tag) {
              dst[existing].tag = seed_start;
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
        add_thread_step(curr, prog_.entry, static_cast<int32_t>(pos), pos);
      }

      // REG_NOSUB and nmatch == 0 request existence only. All public argument
      // and resource checks have already run, and backreferences use the
      // separate executor. Observable spans still require leftmost-longest.
      if (has_match && !need_match_span_) break;
      if (curr.empty() && has_match) break;
      if (pos == len_) break;

      unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (const Thread& t : curr) slot_of[t.pc] = -1;
      for (const Thread& t : curr) {
        if (has_match && t.tag > best_start) continue;
        const Insn& insn = prog_.insns[t.pc];
        if (MatchConsumeOp(insn, ch)) {
          add_thread_step(next, insn.out1, t.tag, pos + 1);
        }
      }
      curr.swap(next);
      next.clear();
    }

    if (!has_match) return kRegNomatch;
    out_start = best_start;
    out_end = best_end;
    if (track_caps_) {
      // Both NFA phases coexist here. Include vector capacity headroom,
      // capture offsets and iterative dissection tasks before input scratch.
      const size_t cap_bytes = num_slots_ * sizeof(int32_t) +
          ninsns * (nfa_bytes_per_state + bytes_per_state) +
          prog_.nodes.size() * sizeof(DissectTask);
      if (cap_bytes > kMaxWorkBytes) return kRegEspace;
      std::vector<int32_t> caps(num_slots_, -1);
      caps[0] = best_start;
      caps[1] = best_end;
      DissectScratch sc(ninsns, kMaxWorkBytes - cap_bytes);
      int rc = Dissect(sc, prog_.root_ast, static_cast<size_t>(best_start),
                       static_cast<size_t>(best_end), caps);
      if (rc != kRegOk) return rc;
      out_caps = std::move(caps);
    }
    return kRegOk;
  }

  // Filter the complete expression using each backreference's copied group
  // language. Branch-local capture dissection is valid only for these spans.
  int CollectCandidateEnds(size_t start, std::vector<size_t>& ends_desc,
                           size_t& steps) const {
    const size_t ninsns = prog_.insns.size();
    // Include capacity headroom for the closure stack and both state lists.
    const size_t scratch_bytes = ninsns * 64 + 256;
    if (scratch_bytes >= kMaxWorkBytes) return kRegEspace;
    const size_t max_ends = (kMaxWorkBytes - scratch_bytes) / sizeof(size_t);
    if (ends_desc.capacity() > max_ends) return kRegEspace;
    ends_desc.clear();
    auto append_end = [&](size_t pos) {
      if (ends_desc.size() == ends_desc.capacity()) {
        const size_t capacity = std::min(max_ends, std::max(size_t{16}, ends_desc.size() * 2));
        if (capacity <= ends_desc.size()) return false;
        ends_desc.reserve(capacity);
        if (ends_desc.capacity() > max_ends) return false;
      }
      ends_desc.push_back(pos);
      return true;
    };
    std::vector<uint8_t> reachable(ninsns, 0);
    std::vector<uint32_t> vis(ninsns, 0);
    std::vector<int32_t> st, curr, next;
    st.reserve(2 * ninsns);
    curr.reserve(ninsns);
    next.reserve(ninsns);
    uint32_t epoch = 0;

    auto close = [&](std::vector<int32_t>& dst, int32_t seed_pc, size_t pos,
                     bool& match_here) {
      st.clear();
      st.push_back(seed_pc);
      if (++epoch == 0) {
        std::fill(vis.begin(), vis.end(), 0);
        ++epoch;
      }
      while (!st.empty()) {
        if (++steps > kMaxBackrefSteps) return false;
        int32_t pc = st.back();
        st.pop_back();
        if (pc < 0 || vis[pc] == epoch) continue;
        vis[pc] = epoch;
        const Insn& insn = prog_.insns[pc];
        switch (insn.op) {
          case Op::kJump:
          case Op::kSave:
          case Op::kPlusBeg:
            st.push_back(insn.out1);
            break;
          case Op::kSplit:
          case Op::kAltSplit:
          case Op::kPlusEnd:
            st.push_back(insn.out2);
            st.push_back(insn.out1);
            break;
          case Op::kBol:
            if (IsBol(pos)) st.push_back(insn.out1);
            break;
          case Op::kEol:
            if (IsEol(pos)) st.push_back(insn.out1);
            break;
          case Op::kWordBeg:
            if (IsWordBeg(pos)) st.push_back(insn.out1);
            break;
          case Op::kWordEnd:
            if (IsWordEnd(pos)) st.push_back(insn.out1);
            break;
          case Op::kMatch:
            match_here = true;
            break;
          case Op::kBackref:
            st.push_back(insn.out2);
            break;
          case Op::kByte:
          case Op::kAny:
          case Op::kClass:
            if (!reachable[pc]) {
              reachable[pc] = 1;
              dst.push_back(pc);
            }
            break;
        }
      }
      return true;
    };

    bool match_at_start = false;
    if (!close(curr, prog_.entry, start, match_at_start)) return kRegEspace;
    if (match_at_start && !append_end(start)) return kRegEspace;
    for (size_t pos = start; pos < len_; ++pos) {
      if (curr.empty()) break;
      const unsigned char ch = static_cast<unsigned char>(begin_[pos]);
      for (int32_t pc : curr) reachable[pc] = 0;
      bool matched = false;
      for (int32_t pc : curr) {
        if (++steps > kMaxBackrefSteps) return kRegEspace;
        const Insn& insn = prog_.insns[pc];
        if (MatchConsumeOp(insn, ch) && !close(next, insn.out1, pos + 1, matched)) {
          return kRegEspace;
        }
      }
      if (matched && !append_end(pos + 1)) return kRegEspace;
      curr.swap(next);
      next.clear();
    }
    std::reverse(ends_desc.begin(), ends_desc.end());
    return kRegOk;
  }

  int ExecuteBackref(std::vector<int32_t>& out_caps, int32_t& out_start, int32_t& out_end) {
    const size_t slots_size = 2 * (prog_.nsub + 1);
    struct Frame {
      int32_t pc;
      int32_t stop_pc;
      size_t pos;
      size_t empty_rec;
      std::vector<int32_t> caps;
      std::vector<size_t> lastpos;
      bool operator==(const Frame&) const = default;
    };
    struct FrameHash {
      size_t operator()(const Frame& f) const {
        size_t h = static_cast<uint32_t>(f.pc);
        auto mix = [&](size_t value) {
          h ^= value + size_t{0x9e3779b9} + (h << 6) + (h >> 2);
        };
        mix(static_cast<uint32_t>(f.stop_pc));
        mix(f.pos);
        mix(f.empty_rec);
        for (int32_t cap : f.caps) mix(static_cast<uint32_t>(cap));
        for (size_t lp : f.lastpos) mix(lp);
        return h;
      }
    };
    const size_t state_bytes = 2 * (sizeof(Frame) + slots_size * sizeof(int32_t) +
        kMaxParseDepth * sizeof(size_t)) + 64;
    std::vector<size_t> candidate_ends;
    size_t steps = 0;

    for (size_t start = 0; start <= len_; ++start) {
      if (prog_.insns[prog_.entry].op == Op::kBol && !IsBol(start)) continue;

      int filter_result = CollectCandidateEnds(start, candidate_ends, steps);
      if (filter_result != kRegOk) return filter_result;
      const size_t end_bytes = candidate_ends.capacity() * sizeof(size_t);
      const size_t state_limit = (kMaxWorkBytes - end_bytes) / state_bytes;
      if (state_limit < 2) return kRegEspace;
      // Release backtracking storage before constructing the next NFA filter.
      std::vector<Frame> pending;
      std::unordered_set<Frame, FrameHash> seen_failed;
      for (size_t target_end : candidate_ends) {
        pending.clear();
        seen_failed.clear();
        pending.push_back(Frame{prog_.entry, -1, start, 0, std::vector<int32_t>(slots_size, -1), {}});

        while (!pending.empty()) {
          Frame current = std::move(pending.back());
          pending.pop_back();
          while (current.pc >= 0) {
            if (++steps > kMaxBackrefSteps) return kRegEspace;
            if (current.pc == current.stop_pc) {
              // NetBSD OCH_ tests a branch against the complete target span,
              // then returns that branch result. OQUEST_ instead includes the
              // rest of the expression in its continuation (e.g. a star).
              if (current.pos != target_end) break;
              out_start = static_cast<int32_t>(start);
              out_end = static_cast<int32_t>(target_end);
              if (track_caps_) {
                current.caps[0] = out_start;
                current.caps[1] = out_end;
                out_caps = std::move(current.caps);
              }
              return kRegOk;
            }
            const Insn& insn = prog_.insns[current.pc];
            switch (insn.op) {
              case Op::kMatch:
                if (current.pos == target_end) {
                  out_start = static_cast<int32_t>(start);
                  out_end = static_cast<int32_t>(target_end);
                  if (track_caps_) {
                    current.caps[0] = out_start;
                    current.caps[1] = out_end;
                    out_caps = std::move(current.caps);
                  }
                  return kRegOk;
                }
                current.pc = -1;
                break;
              case Op::kJump:
                current.pc = insn.out1;
                break;
              case Op::kPlusBeg:
                if (current.lastpos.size() >= kMaxParseDepth) return kRegEspace;
                current.lastpos.push_back(current.pos);
                current.pc = insn.out1;
                break;
              case Op::kPlusEnd:
                if (current.lastpos.empty()) {
                  current.pc = -1;
                  break;
                }
                if (seen_failed.find(current) != seen_failed.end()) {
                  current.pc = -1;
                  break;
                }
                if (pending.size() + seen_failed.size() + 2 > state_limit) return kRegEspace;
                seen_failed.insert(current);
                if (current.pos == current.lastpos.back()) {
                  // Previous pass matched empty: exit loop to break zero-width repetition.
                  current.lastpos.pop_back();
                  current.pc = insn.out2;
                } else {
                  // NetBSD backref() tries another pass first, falling back to loop exit.
                  Frame exit_frame{insn.out2, current.stop_pc, current.pos, current.empty_rec, current.caps,
                                   current.lastpos};
                  exit_frame.lastpos.pop_back();
                  pending.push_back(std::move(exit_frame));
                  current.lastpos.back() = current.pos;
                  current.pc = insn.out1;
                }
                break;
              case Op::kAltSplit:
                current.stop_pc = insn.arg;
                [[fallthrough]];
              case Op::kSplit:
                if (seen_failed.find(current) != seen_failed.end()) {
                  current.pc = -1;
                  break;
                }
                if (pending.size() + seen_failed.size() + 2 > state_limit) return kRegEspace;
                seen_failed.insert(current);
                pending.push_back(Frame{insn.out2, current.stop_pc, current.pos, current.empty_rec, current.caps,
                                        current.lastpos});
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
                if (static_cast<size_t>(insn.arg) < current.caps.size()) {
                  current.caps[insn.arg] = static_cast<int32_t>(current.pos);
                }
                current.pc = insn.out1;
                break;
              case Op::kByte:
              case Op::kAny:
              case Op::kClass:
                if (current.pos < target_end &&
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
                if (length == 0 && ++current.empty_rec > kMaxEmptyBackrefRecursion) {
                  current.pc = -1;
                  break;
                }
                bool matches = length <= target_end - current.pos;
                for (size_t i = 0; matches && i < length; ++i) {
                  if (++steps > kMaxBackrefSteps) return kRegEspace;
                  const auto expected = static_cast<unsigned char>(begin_[rso + i]);
                  const auto actual = static_cast<unsigned char>(begin_[current.pos + i]);
                  // AOSP's OBACK_ compares captured bytes literally even with REG_ICASE.
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
      }
    }
    return kRegNomatch;
  }

  const Program& prog_;
  const char* base_;
  const char* begin_;
  size_t len_;
  int eflags_;
  bool need_match_span_;
  bool track_caps_;
  size_t num_slots_;
};

}  // namespace gesher::bionic::posix_regex
