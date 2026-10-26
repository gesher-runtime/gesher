// gesher-exec's /system/bin/sh (gesher_shell.h, docs/exec.md, ADR 0060).
//
// A POSIX shell with the mksh extensions Android scripts use, built for
// Windows, where there is no fork:
//
//   - input is read and run one complete command at a time (a program writing
//     commands to the shell's stdin sees each one's output before the next);
//   - the lexer keeps words raw (quotes included); the parser builds a tree of
//     simple commands, pipelines, and-or lists, lists, (subshells), { groups },
//     if/while/until/for/case, and functions;
//   - words are expanded when run: tilde, parameters (${x:-w}, ${#x}, ${x#p},
//     ${x%p}, ${x/p/r}, ${x:o:l} ...), $(...) and `...`, $((...)), field
//     splitting on IFS, pathname globbing, quote removal;
//   - a subshell, a pipeline stage and a command substitution run a copy of the
//     shell's state (variables, functions, the working directory, which the
//     shell keeps itself rather than only in the process) on a thread of their
//     own; pipeline stages run at once, joined by Windows pipes;
//   - other commands are Windows processes, found as execvp finds them
//     (exec_spawn.h: Android ELF programs through gesher-exec, the /system/bin
//     tools, Windows programs), with the shell's fds 0-9 and its exported
//     variables;
//   - a signal another process sends to the shell (gesher-base's mailbox:
//     Process.destroy() is SIGTERM) runs its trap, or is passed to the
//     foreground programs and ends the shell with 128 + the signal.
//
// Paths are Linux paths as gesher-base maps them: "/x" is on the current
// drive, "/mnt/d/x" on drive D:, /dev/null is NUL.

#include "gesher_shell.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "exec_spawn.h"

// gesher-base (compat/posix/src/process.cc): signals to other processes, and
// the hook that gets the signals other processes send this one.
extern "C" int gesher_kill_other(int pid, int sig);
extern "C" int (*gesher_signal_hook)(int sig, int sender_pid);

namespace gesher::exec {

namespace {

// --- small helpers -------------------------------------------------------------

bool IsNameStart(char c) { return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }
bool IsDigitC(char c) { return c >= '0' && c <= '9'; }
bool IsName(const std::string& s) {
  if (s.empty() || !IsNameStart(s[0])) return false;
  for (char c : s) {
    if (!IsNameChar(c)) return false;
  }
  return true;
}
bool IsNumber(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!IsDigitC(c)) return false;
  }
  return true;
}

std::string Format(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[2048];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return "";
  return std::string(buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
}

// Writes all of `s` to `h`; false if the handle is closed or broken (EPIPE).
bool WriteAll(HANDLE h, const std::string& s) {
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  while (done < s.size()) {
    DWORD wrote = 0;
    DWORD chunk = static_cast<DWORD>(std::min<size_t>(s.size() - done, 1u << 20));
    if (!WriteFile(h, s.data() + done, chunk, &wrote, nullptr) || wrote == 0) return false;
    done += wrote;
  }
  return true;
}

// --- signals ---------------------------------------------------------------------
//
// Process-wide: what the main shell's `trap` set, the foreground programs (to
// pass a fatal signal on), and the signals waiting for their trap.

constexpr int kMaxSig = 65;
std::atomic<int> g_trap_mode[kMaxSig];  // 0 default, 1 ignored, 2 trapped
std::atomic<uint64_t> g_pending_signals{0};
std::mutex g_fg_mutex;
std::set<DWORD> g_fg_pids;

struct SigName {
  const char* name;
  int sig;
};
const SigName kSignals[] = {{"HUP", 1},     {"INT", 2},   {"QUIT", 3},  {"ILL", 4},   {"TRAP", 5},
                            {"ABRT", 6},    {"BUS", 7},   {"FPE", 8},   {"KILL", 9},  {"USR1", 10},
                            {"SEGV", 11},   {"USR2", 12}, {"PIPE", 13}, {"ALRM", 14}, {"TERM", 15},
                            {"STKFLT", 16}, {"CHLD", 17}, {"CONT", 18}, {"STOP", 19}, {"TSTP", 20},
                            {"TTIN", 21},   {"TTOU", 22}, {"URG", 23},  {"XCPU", 24}, {"XFSZ", 25},
                            {"VTALRM", 26}, {"PROF", 27}, {"WINCH", 28}, {"IO", 29},  {"PWR", 30},
                            {"SYS", 31}};

// A signal by name (TERM, SIGTERM, term) or number; -1 if unknown. "EXIT" is 0.
int SignalNumber(const std::string& s) {
  if (IsNumber(s)) {
    int n = atoi(s.c_str());
    return n >= 0 && n < kMaxSig ? n : -1;
  }
  std::string n = s;
  for (char& c : n) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  if (n.rfind("SIG", 0) == 0) n = n.substr(3);
  if (n == "EXIT") return 0;
  for (const SigName& sn : kSignals) {
    if (n == sn.name) return sn.sig;
  }
  return -1;
}

bool DefaultIgnored(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }

std::atomic<bool> g_have_tail_child{false};  // the foreground program decides the status

// gesher-base calls this (on a thread-pool thread) for each signal another
// process sends. Returns nonzero: the shell owns the signal.
int ShellSignalHook(int sig, int /*sender*/) {
  if (sig <= 0 || sig >= kMaxSig) return 0;
  int mode = g_trap_mode[sig].load();
  if (mode == 1) return 1;  // ignored
  if (mode == 2) {
    g_pending_signals.fetch_or(uint64_t{1} << sig);
    return 1;
  }
  if (DefaultIgnored(sig)) return 1;
  std::vector<DWORD> fg;
  {
    std::lock_guard<std::mutex> lock(g_fg_mutex);
    fg.assign(g_fg_pids.begin(), g_fg_pids.end());
  }
  for (DWORD pid : fg) gesher_kill_other(static_cast<int>(pid), sig);
  if (!fg.empty() && g_have_tail_child.load()) return 1;  // the program handles it
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(128 + sig));
  return 1;
}

void AddForeground(DWORD pid) {
  std::lock_guard<std::mutex> lock(g_fg_mutex);
  g_fg_pids.insert(pid);
}
void RemoveForeground(DWORD pid) {
  std::lock_guard<std::mutex> lock(g_fg_mutex);
  g_fg_pids.erase(pid);
}

// --- the state -----------------------------------------------------------------

struct Var {
  std::string value;
  bool exported = false;
  bool readonly = false;
};

struct Node;
using NodeP = std::shared_ptr<Node>;

// A handle the shell opened (a redirection's file, a pipe end): closed when the
// last user lets go.
struct OwnedHandle {
  HANDLE h = nullptr;
  explicit OwnedHandle(HANDLE handle) : h(handle) {}
  ~OwnedHandle() {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
};
using HandleP = std::shared_ptr<OwnedHandle>;

// The shell's file descriptors 0-9 for a command: the raw handle and a
// shared_ptr that keeps an opened one alive.
struct Io {
  HANDLE fd[10] = {};
  HandleP owned[10];
  Io() {}
  void set(int n, HANDLE h, HandleP keep = nullptr) {
    fd[n] = h;
    owned[n] = keep;
  }
};

struct Shell {
  std::map<std::string, Var> vars;
  std::string cwd;
  Io io;
  std::vector<std::string> pos;  // $0, $1...
  std::map<std::string, NodeP> funcs;
  std::map<DWORD, HandleP> jobs;
  std::map<int, std::string> traps;
  bool opt_e = false, opt_u = false, opt_x = false, opt_f = false, opt_pipefail = false;
  int status = 0;
  DWORD last_bg = 0;
  bool exiting = false;
  int breaking = 0, continuing = 0;
  bool returning = false;
  int loop_depth = 0, func_depth = 0;
  int cond_depth = 0;  // inside a condition: set -e does not apply
  bool is_subshell = false;
  // `exec N>file`: redirections that outlast the command.
  std::map<int, HandleP> persistent_fd;
  std::string name = "sh";
  time_t started = time(nullptr);
  // Saved variable scopes for `local`.
  std::vector<std::vector<std::pair<std::string, std::optional<Var>>>> local_scopes;
};

// --- variables -----------------------------------------------------------------

const std::string* GetVarPtr(const Shell& sh, const std::string& name) {
  auto it = sh.vars.find(name);
  return it == sh.vars.end() ? nullptr : &it->second.value;
}

std::string SpecialVar(const Shell& sh, const std::string& name);

std::string GetVar(const Shell& sh, const std::string& name) {
  if (!name.empty() && !IsNameStart(name[0])) return SpecialVar(sh, name);
  if (IsNumber(name)) return SpecialVar(sh, name);
  const std::string* v = GetVarPtr(sh, name);
  return v != nullptr ? *v : "";
}

bool VarSet(const Shell& sh, const std::string& name) {
  if (name == "?" || name == "#" || name == "$" || name == "!" || name == "@" || name == "*" ||
      name == "-" || name == "0")
    return true;
  if (IsNumber(name)) {
    size_t i = static_cast<size_t>(atoi(name.c_str()));
    return i < sh.pos.size();
  }
  return sh.vars.count(name) != 0;
}

void SetVar(Shell* sh, const std::string& name, const std::string& value) {
  Var& v = sh->vars[name];
  if (v.readonly) return;
  v.value = value;
}

void ExportVar(Shell* sh, const std::string& name) {
  Var& v = sh->vars[name];
  v.exported = true;
}

std::string Ifs(const Shell& sh) {
  const std::string* v = GetVarPtr(sh, "IFS");
  return v != nullptr ? *v : " \t\n";
}

std::string SpecialVar(const Shell& sh, const std::string& name) {
  if (name == "?") return std::to_string(sh.status);
  if (name == "#") return std::to_string(sh.pos.empty() ? 0 : sh.pos.size() - 1);
  if (name == "$") return std::to_string(static_cast<int>(GetCurrentProcessId()));
  if (name == "!") return std::to_string(sh.last_bg);
  if (name == "-") {
    std::string s;
    if (sh.opt_e) s += 'e';
    if (sh.opt_u) s += 'u';
    if (sh.opt_x) s += 'x';
    if (sh.opt_f) s += 'f';
    return s;
  }
  if (name == "@" || name == "*") {
    std::string out;
    std::string sep = name == "*" ? std::string(1, Ifs(sh).empty() ? ' ' : Ifs(sh)[0]) : " ";
    for (size_t i = 1; i < sh.pos.size(); ++i) {
      if (i > 1) out += sep;
      out += sh.pos[i];
    }
    return out;
  }
  if (IsNumber(name)) {
    size_t i = static_cast<size_t>(atoi(name.c_str()));
    return i < sh.pos.size() ? sh.pos[i] : "";
  }
  return "";
}

// The inherited environment is imported into vars when the shell starts.
// Keeping the environment here also makes unset and subshells independent.
std::vector<std::string> ChildEnv(const Shell& sh) {
  std::map<std::string, std::string> env;
  for (const auto& kv : sh.vars) {
    if (kv.second.exported) env[kv.first] = kv.second.value;
  }
  std::vector<std::string> out;
  out.reserve(env.size());
  for (const auto& kv : env) out.push_back(kv.first + "=" + kv.second);
  return out;
}

// --- the working directory -----------------------------------------------------

std::string CurrentDir() {
  DWORD n = GetCurrentDirectoryW(0, nullptr);
  std::wstring d(n, L'\0');
  n = GetCurrentDirectoryW(n, d.data());
  d.resize(n);
  return Narrow(d);
}

void Err(const Io& io, const std::string& msg) {
  HANDLE h = io.fd[2];
  if (h != nullptr && h != INVALID_HANDLE_VALUE) WriteAll(h, msg);
}
void ShErr(const Shell& sh, const Io& io, const std::string& msg) {
  Err(io, sh.name + ": " + msg + "\n");
}

// --- the lexer -----------------------------------------------------------------

enum class Tok { Word, Op, Newline, Eof };

struct Token {
  Tok type = Tok::Eof;
  std::string text;       // the raw word (quotes kept) or the operator
  bool quoted = false;    // the word had quotes or a backslash (not a keyword)
  std::string heredoc;    // a << operator's body
  bool heredoc_expand = true;
  int io_number = -1;     // a redirection's explicit fd, or -1
  bool incomplete = false;
};

// Scans one raw word from src[*i], honouring '', "", \, and the $(...), ${...},
// `...` and $((...)) forms that may hold spaces and operators. Advances *i.
void ScanBalanced(const std::string& src, size_t* i, char open, char close, bool* incomplete);

void ScanQuoted(const std::string& src, size_t* i, bool* incomplete) {
  char quote = src[(*i)++];
  while (*i < src.size()) {
    char c = src[(*i)++];
    if (c == quote) return;
    if (quote != '\'' && c == '\\') {
      if (*i < src.size()) ++*i;
    } else if (quote == '"' && c == '$' && *i < src.size()) {
      if (src[*i] == '(') ScanBalanced(src, i, '(', ')', incomplete);
      else if (src[*i] == '{') ScanBalanced(src, i, '{', '}', incomplete);
    } else if (quote == '"' && c == '`') {
      --*i;
      ScanQuoted(src, i, incomplete);
    }
  }
  if (incomplete != nullptr) *incomplete = true;
}

void ScanBalanced(const std::string& src, size_t* i, char open, char close, bool* incomplete) {
  int depth = 0;
  while (*i < src.size()) {
    char c = src[*i];
    if (c == '\'' || c == '"' || c == '`') {
      ScanQuoted(src, i, incomplete);
    } else if (c == '\\') {
      *i += std::min<size_t>(2, src.size() - *i);
    } else if (c == '$' && *i + 1 < src.size() && src[*i + 1] == '(') {
      ++*i;
      ScanBalanced(src, i, '(', ')', incomplete);
    } else if (c == '$' && *i + 1 < src.size() && src[*i + 1] == '{') {
      ++*i;
      ScanBalanced(src, i, '{', '}', incomplete);
    } else {
      ++*i;
      if (c == open) ++depth;
      else if (c == close && --depth == 0) return;
    }
  }
  if (incomplete != nullptr) *incomplete = true;
}

std::string ScanWord(const std::string& src, size_t* i, bool* quoted, bool* incomplete = nullptr) {
  size_t start = *i;
  size_t n = src.size();
  while (*i < n) {
    char c = src[*i];
    if (c == '\'' || c == '"') {
      *quoted = true;
      ScanQuoted(src, i, incomplete);
    } else if (c == '\\') {
      *quoted = true;
      *i += std::min<size_t>(2, n - *i);
    } else if (c == '$' && *i + 1 < n && src[*i + 1] == '(') {
      ++*i;
      ScanBalanced(src, i, '(', ')', incomplete);
    } else if (c == '$' && *i + 1 < n && src[*i + 1] == '{') {
      ++*i;
      ScanBalanced(src, i, '{', '}', incomplete);
    } else if (c == '`') {
      ScanQuoted(src, i, incomplete);
    } else if (strchr(" \t\n;|&<>()", c) != nullptr) {
      break;
    } else {
      ++*i;
    }
  }
  return src.substr(start, *i - start);
}

std::string ScanRedirOp(const std::string& src, size_t* i) {
  size_t n = src.size();
  char c = src[*i];
  if (c == '<') {
    if (*i + 1 < n && src[*i + 1] == '<') {
      if (*i + 2 < n && src[*i + 2] == '-') return *i += 3, "<<-";
      return *i += 2, "<<";
    }
    if (*i + 1 < n && src[*i + 1] == '&') return *i += 2, "<&";
    if (*i + 1 < n && src[*i + 1] == '>') return *i += 2, "<>";
    return ++*i, "<";
  }
  if (c == '>') {
    if (*i + 1 < n && src[*i + 1] == '>') return *i += 2, ">>";
    if (*i + 1 < n && src[*i + 1] == '&') return *i += 2, ">&";
    if (*i + 1 < n && src[*i + 1] == '|') return *i += 2, ">|";
    return ++*i, ">";
  }
  return "";
}

std::vector<Token> Tokenize(const std::string& src) {
  std::vector<Token> out;
  size_t i = 0, n = src.size();
  std::vector<size_t> pending_heredocs;  // indices in out of << tokens
  auto collect_heredocs = [&](size_t from) {
    size_t p = from;
    for (size_t idx : pending_heredocs) {
      std::string delim = out[idx].text;  // raw delimiter, possibly quoted
      bool dash = out[idx].io_number == -2;
      bool quoted = delim.find_first_of("'\"\\") != std::string::npos;
      std::string bare;
      for (char ch : delim) {
        if (ch != '\'' && ch != '"' && ch != '\\') bare += ch;
      }
      std::string body;
      bool found = false;
      while (p < n) {
        size_t le = src.find('\n', p);
        std::string line = src.substr(p, (le == std::string::npos ? n : le) - p);
        std::string trimmed = line;
        if (dash) {
          size_t t = trimmed.find_first_not_of('\t');
          trimmed = t == std::string::npos ? "" : trimmed.substr(t);
        }
        if (trimmed == bare) {
          p = (le == std::string::npos ? n : le + 1);
          found = true;
          break;
        }
        body += dash ? trimmed : line;
        body += '\n';
        if (le == std::string::npos) {
          p = n;
          break;
        }
        p = le + 1;
      }
      out[idx].heredoc = body;
      out[idx].heredoc_expand = !quoted;
      out[idx].incomplete = !found;
      out[idx].text = "<<";
      out[idx].io_number = out[idx].io_number == -2 ? -1 : out[idx].io_number;
    }
    pending_heredocs.clear();
    i = p;
  };
  while (i < n) {
    char c = src[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '#') {
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    if (c == '\n') {
      ++i;
      out.push_back({Tok::Newline, "\n", false, "", true, -1});
      if (!pending_heredocs.empty()) collect_heredocs(i);
      continue;
    }
    if (c == ';') {
      if (i + 1 < n && src[i + 1] == ';') {
        out.push_back({Tok::Op, ";;", false, "", true, -1});
        i += 2;
      } else {
        out.push_back({Tok::Op, ";", false, "", true, -1});
        ++i;
      }
      continue;
    }
    if (c == '&') {
      if (i + 1 < n && src[i + 1] == '&') {
        out.push_back({Tok::Op, "&&", false, "", true, -1});
        i += 2;
      } else {
        out.push_back({Tok::Op, "&", false, "", true, -1});
        ++i;
      }
      continue;
    }
    if (c == '|') {
      if (i + 1 < n && src[i + 1] == '|') {
        out.push_back({Tok::Op, "||", false, "", true, -1});
        i += 2;
      } else {
        out.push_back({Tok::Op, "|", false, "", true, -1});
        ++i;
      }
      continue;
    }
    if (c == '(' || c == ')') {
      out.push_back({Tok::Op, std::string(1, c), false, "", true, -1});
      ++i;
      continue;
    }
    // A redirection, possibly with a leading fd number.
    size_t j = i;
    std::string digits;
    while (j < n && IsDigitC(src[j])) digits += src[j++];
    if (j < n && (src[j] == '<' || src[j] == '>')) {
      size_t k = j;
      std::string op = ScanRedirOp(src, &k);
      if (!op.empty()) {
        i = k;
        int ionum = digits.empty() ? -1 : atoi(digits.c_str());
        if (op == "<<" || op == "<<-") {
          while (i < n && (src[i] == ' ' || src[i] == '\t')) ++i;
          bool hq = false;
          std::string delim = ScanWord(src, &i, &hq);
          Token t{Tok::Op, delim, false, "", true, op == "<<-" ? -2 : ionum};
          out.push_back(t);
          pending_heredocs.push_back(out.size() - 1);
        } else {
          Token t{Tok::Op, op, false, "", true, ionum};
          out.push_back(t);
        }
        continue;
      }
    }
    bool hq = false;
    bool incomplete = false;
    std::string w = ScanWord(src, &i, &hq, &incomplete);
    if (w.empty()) {
      ++i;
      continue;
    }
    out.push_back({Tok::Word, w, hq, "", true, -1, incomplete});
  }
  for (size_t idx : pending_heredocs) out[idx].incomplete = true;
  out.push_back({Tok::Eof, "", false, "", true, -1});
  return out;
}

// --- the AST -------------------------------------------------------------------

struct Redir {
  int fd = -1;        // the fd being redirected (filled by default per op)
  std::string op;     // "<", ">", ">>", "<&", ">&", "<>", ">|", "<<"
  std::string target; // a raw word, or an fd/"-" for <& >&, or the here-doc body
  bool heredoc_expand = true;
};

struct Node {
  enum Kind { Simple, Pipeline, AndOr, List, If, For, While, Case, Group, Subshell, FuncDef, Arith } kind;
  // Simple:
  std::vector<std::string> words;                             // raw, unexpanded
  std::vector<std::pair<std::string, std::string>> assigns;   // NAME=rawvalue
  std::vector<Redir> redirs;
  // Pipeline:
  bool negate = false;
  // AndOr: children with connectors between them.
  std::vector<std::string> connectors;
  // List: children with terminators (";", "&", "\n").
  std::vector<std::string> terminators;
  std::vector<NodeP> children;
  // If: cond/body, then flattened elif cond,body pairs, then else.
  std::vector<NodeP> elifs;
  NodeP else_body;
  // For:
  std::string var;
  std::vector<std::string> for_words;
  bool for_in = false;
  NodeP body;
  // While/Until:
  bool until = false;
  NodeP cond;
  // Case:
  std::string case_word;
  std::vector<std::pair<std::vector<std::string>, NodeP>> cases;
  // FuncDef:
  std::string func_name;
};

// --- the parser ----------------------------------------------------------------

struct Parser {
  const std::vector<Token>& toks;
  size_t pos = 0;
  std::string error;

  explicit Parser(const std::vector<Token>& t) : toks(t) {}

  const Token& peek() const { return toks[pos]; }
  const Token& at(size_t k) const { return pos + k < toks.size() ? toks[pos + k] : toks.back(); }
  void advance() {
    if (pos + 1 < toks.size()) ++pos;
  }
  bool isWord(const char* s) const {
    return peek().type == Tok::Word && !peek().quoted && peek().text == s;
  }
  bool isOp(const char* s) const { return peek().type == Tok::Op && peek().text == s; }
  bool atEof() const { return peek().type == Tok::Eof; }

  void skipNewlines() {
    while (peek().type == Tok::Newline) advance();
  }

  bool isReserved(const std::string& w) const {
    static const char* const kw[] = {"if", "then", "elif", "else", "fi",   "for", "in",   "do",
                                      "done", "while", "until", "case", "esac", "{",   "}",    "!",
                                      "function"};
    for (const char* k : kw) {
      if (w == k) return true;
    }
    return false;
  }
  bool isTerminatorWord() const {
    if (peek().type != Tok::Word || peek().quoted) return false;
    const std::string& w = peek().text;
    return w == "then" || w == "else" || w == "elif" || w == "fi" || w == "do" || w == "done" ||
           w == "esac" || w == "}";
  }

  NodeP parseProgram() {
    for (const Token& t : toks) {
      if (t.incomplete) {
        error = "unexpected end of input";
        return nullptr;
      }
    }
    NodeP list = parseList();
    if (!atEof() && error.empty()) error = "unexpected token near `" + peek().text + "'";
    return list;
  }

  NodeP parseList() {
    auto node = std::make_shared<Node>();
    node->kind = Node::List;
    skipNewlines();
    while (!atEof() && error.empty()) {
      if (isTerminatorWord() || isOp(")") || isOp(";;")) break;
      NodeP ao = parseAndOr();
      if (!ao) break;
      node->children.push_back(ao);
      std::string term = "\n";
      if (isOp("&")) {
        term = "&";
        advance();
      } else if (isOp(";")) {
        term = ";";
        advance();
      } else if (peek().type == Tok::Newline) {
        term = "\n";
      } else {
        node->terminators.push_back(term);
        break;
      }
      node->terminators.push_back(term);
      skipNewlines();
    }
    if (node->children.size() == 1 &&
        (node->terminators.empty() || node->terminators[0] != "&")) {
      return node->children[0];
    }
    return node;
  }

  NodeP parseAndOr() {
    NodeP first = parsePipeline();
    if (!first) return nullptr;
    if (!isOp("&&") && !isOp("||")) return first;
    auto node = std::make_shared<Node>();
    node->kind = Node::AndOr;
    node->children.push_back(first);
    while (isOp("&&") || isOp("||")) {
      node->connectors.push_back(peek().text);
      advance();
      skipNewlines();
      NodeP next = parsePipeline();
      if (!next) break;
      node->children.push_back(next);
    }
    return node;
  }

  NodeP parsePipeline() {
    bool negate = false;
    while (isWord("!")) {
      negate = !negate;
      advance();
    }
    NodeP first = parseCommand();
    if (!first) return nullptr;
    if (!isOp("|")) {
      if (negate) {
        auto p = std::make_shared<Node>();
        p->kind = Node::Pipeline;
        p->negate = true;
        p->children.push_back(first);
        return p;
      }
      return first;
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Pipeline;
    node->negate = negate;
    node->children.push_back(first);
    while (isOp("|")) {
      advance();
      skipNewlines();
      NodeP next = parseCommand();
      if (!next) break;
      node->children.push_back(next);
    }
    return node;
  }

  NodeP parseCommand() {
    if (isWord("if")) return parseIf();
    if (isWord("for")) return parseFor();
    if (isWord("while") || isWord("until")) return parseWhile();
    if (isWord("case")) return parseCase();
    if (isOp("{")) return parseGroup();
    if (isWord("{")) return parseGroup();
    if (isOp("(")) return parseSubshell();
    if (peek().type == Tok::Word && !peek().quoted && !isReserved(peek().text) &&
        at(1).type == Tok::Op && at(1).text == "(" && at(2).type == Tok::Op && at(2).text == ")") {
      return parseFuncDef(peek().text);
    }
    if (isWord("function")) {
      advance();
      std::string name = peek().text;
      if (peek().quoted || !IsName(name)) {
        error = "function: expected a name";
        return nullptr;
      }
      return parseFuncDef(name);
    }
    return parseSimple();
  }

  void parseRedirsInto(std::vector<Redir>* redirs) {
    while (peek().type == Tok::Op) {
      const std::string& op = peek().text;
      if (op != "<" && op != ">" && op != ">>" && op != "<&" && op != ">&" && op != "<>" &&
          op != ">|" && op != "<<") {
        break;
      }
      Token rt = peek();
      advance();
      Redir r;
      r.op = op;
      bool input = (op == "<" || op == "<<" || op == "<&");
      r.fd = rt.io_number >= 0 ? rt.io_number : (input ? 0 : 1);
      if (op == "<<") {
        r.target = rt.heredoc;
        r.heredoc_expand = rt.heredoc_expand;
      } else {
        if (peek().type != Tok::Word) {
          error = "expected a filename after `" + op + "'";
          return;
        }
        r.target = peek().text;
        advance();
      }
      redirs->push_back(r);
    }
  }

  NodeP parseSimple() {
    auto node = std::make_shared<Node>();
    node->kind = Node::Simple;
    bool seen_word = false;
    while (!atEof() && error.empty()) {
      if (peek().type == Tok::Op) {
        const std::string& t = peek().text;
        if (t == ";" || t == "&" || t == "&&" || t == "||" || t == "|" || t == ")" || t == ";;") break;
        size_t before = pos;
        parseRedirsInto(&node->redirs);
        if (pos != before) continue;
        break;
      }
      if (peek().type == Tok::Newline) break;
      if (peek().type == Tok::Word) {
        const std::string& w = peek().text;
        if (!seen_word && !peek().quoted && isTerminatorWord()) break;
        if (!seen_word) {
          size_t eq = w.find('=');
          if (eq != std::string::npos && eq > 0 && IsNameStart(w[0])) {
            bool ok = true;
            for (size_t x = 0; x < eq; ++x) {
              if (!IsNameChar(w[x])) {
                ok = false;
                break;
              }
            }
            if (ok) {
              node->assigns.push_back({w.substr(0, eq), w.substr(eq + 1)});
              advance();
              continue;
            }
          }
        }
        node->words.push_back(w);
        seen_word = true;
        advance();
        continue;
      }
      break;
    }
    return node;
  }

  NodeP parseGroup() {
    advance();  // { or "{"
    auto body = parseList();
    if (!(peek().type == Tok::Word && peek().text == "}")) {
      if (error.empty()) error = "expected `}'";
    } else {
      advance();
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Group;
    node->body = body;
    parseRedirsInto(&node->redirs);
    return node;
  }

  NodeP parseSubshell() {
    advance();  // (
    auto body = parseList();
    if (!isOp(")")) {
      if (error.empty()) error = "expected `)'";
    } else {
      advance();
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Subshell;
    node->body = body;
    parseRedirsInto(&node->redirs);
    return node;
  }

  NodeP parseFuncDef(const std::string& name) {
    auto node = std::make_shared<Node>();
    node->kind = Node::FuncDef;
    node->func_name = name;
    advance();  // name
    if (isOp("(")) {
      advance();
      if (isOp(")")) advance();
    }
    skipNewlines();
    node->body = parseCommand();
    return node;
  }

  void expect(const char* w) {
    if (peek().type == Tok::Word && peek().text == w) {
      advance();
    } else if (error.empty()) {
      error = std::string("expected `") + w + "'" + (peek().text.empty() ? "" : ", got `" + peek().text + "'");
    }
  }

  NodeP parseIf() {
    advance();  // if
    auto node = std::make_shared<Node>();
    node->kind = Node::If;
    node->cond = parseList();
    expect("then");
    node->body = parseList();
    while (isWord("elif")) {
      advance();
      node->elifs.push_back(parseList());
      expect("then");
      node->elifs.push_back(parseList());
    }
    if (isWord("else")) {
      advance();
      node->else_body = parseList();
    }
    expect("fi");
    parseRedirsInto(&node->redirs);
    return node;
  }

  NodeP parseFor() {
    advance();  // for
    auto node = std::make_shared<Node>();
    node->kind = Node::For;
    if (peek().type != Tok::Word) {
      error = "for: expected a variable name";
      return node;
    }
    node->var = peek().text;
    advance();
    skipNewlines();
    if (isWord("in")) {
      node->for_in = true;
      advance();
      while (peek().type == Tok::Word && !isTerminatorWord()) {
        node->for_words.push_back(peek().text);
        advance();
      }
      if (isOp(";")) advance();
      skipNewlines();
    } else {
      if (isOp(";")) advance();
      skipNewlines();
    }
    expect("do");
    node->body = parseList();
    expect("done");
    parseRedirsInto(&node->redirs);
    return node;
  }

  NodeP parseWhile() {
    auto node = std::make_shared<Node>();
    node->kind = Node::While;
    node->until = isWord("until");
    advance();  // while/until
    node->cond = parseList();
    expect("do");
    node->body = parseList();
    expect("done");
    parseRedirsInto(&node->redirs);
    return node;
  }

  NodeP parseCase() {
    advance();  // case
    auto node = std::make_shared<Node>();
    node->kind = Node::Case;
    if (peek().type != Tok::Word) {
      error = "case: expected a word";
      return node;
    }
    node->case_word = peek().text;
    advance();
    skipNewlines();
    expect("in");
    skipNewlines();
    while (!isWord("esac") && !atEof() && error.empty()) {
      if (isOp("(")) advance();
      std::vector<std::string> pats;
      pats.push_back(peek().text);
      advance();
      while (isOp("|")) {
        advance();
        pats.push_back(peek().text);
        advance();
      }
      if (!isOp(")")) {
        error = "case: expected `)'";
        break;
      }
      advance();
      NodeP body = parseList();
      node->cases.push_back({pats, body});
      if (isOp(";;")) advance();
      skipNewlines();
    }
    expect("esac");
    parseRedirsInto(&node->redirs);
    return node;
  }
};

// --- execution forward declarations --------------------------------------------

int ExecNode(Shell* sh, const NodeP& node, Io io);
int RunProgramString(Shell* sh, const std::string& src, Io io);
int RunSubshell(Shell* sh, const std::string& src, Io io, bool background = false);

// Runs `src` as a subshell and returns its standard output (trailing newlines
// stripped), through a temp file (output is finite).
std::string CommandSubstitute(Shell* sh, const std::string& src) {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  wchar_t path[MAX_PATH];
  GetTempFileNameW(dir, L"gsh", 0, path);
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  std::string result;
  if (h != INVALID_HANDLE_VALUE) {
    Io io = sh->io;
    io.set(1, h);
    sh->status = RunSubshell(sh, src, io);
    FlushFileBuffers(h);
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) result.append(buf, got);
    CloseHandle(h);
  }
  DeleteFileW(path);
  while (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

// --- arithmetic ($(( )) and let) -----------------------------------------------

struct Arith {
  const std::string& s;
  size_t i = 0;
  Shell* sh;
  bool ok = true;
  Arith(const std::string& str, Shell* shell) : s(str), sh(shell) {}
  void skip() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  }
  long long atom() {
    skip();
    if (i < s.size() && s[i] == '(') {
      ++i;
      long long v = expr(0);
      skip();
      if (i < s.size() && s[i] == ')') ++i;
      return v;
    }
    if (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == '!' || s[i] == '~')) {
      char op = s[i++];
      long long v = atom();
      return op == '-' ? -v : op == '!' ? !v : op == '~' ? ~v : v;
    }
    if (i < s.size() && IsDigitC(s[i])) {
      // Support 0x.. and 0.. octal loosely via strtoll.
      char* end = nullptr;
      long long v = strtoll(s.c_str() + i, &end, 0);
      i = static_cast<size_t>(end - s.c_str());
      return v;
    }
    if (i < s.size() && IsNameStart(s[i])) {
      std::string name;
      while (i < s.size() && IsNameChar(s[i])) name += s[i++];
      skip();
      if (i < s.size() && s[i] == '=' && (i + 1 >= s.size() || s[i + 1] != '=')) {
        ++i;
        long long v = expr(0);
        SetVar(sh, name, std::to_string(v));
        return v;
      }
      return atoll(GetVar(*sh, name).c_str());
    }
    ok = false;
    return 0;
  }
  int prec(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return -1;
  }
  std::string peekOp() {
    skip();
    if (i >= s.size()) return "";
    static const char* const ops[] = {"||", "&&", "==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%"};
    for (const char* o : ops) {
      size_t len = strlen(o);
      if (s.compare(i, len, o) == 0) return o;
    }
    return "";
  }
  long long expr(int min_prec) {
    long long lhs = atom();
    for (;;) {
      std::string op = peekOp();
      int p = prec(op);
      if (p < 0 || p < min_prec) break;
      i += op.size();
      long long rhs = expr(p + 1);
      if (op == "+") lhs += rhs;
      else if (op == "-") lhs -= rhs;
      else if (op == "*") lhs *= rhs;
      else if (op == "/") lhs = rhs != 0 ? lhs / rhs : 0;
      else if (op == "%") lhs = rhs != 0 ? lhs % rhs : 0;
      else if (op == "==") lhs = lhs == rhs;
      else if (op == "!=") lhs = lhs != rhs;
      else if (op == "<") lhs = lhs < rhs;
      else if (op == ">") lhs = lhs > rhs;
      else if (op == "<=") lhs = lhs <= rhs;
      else if (op == ">=") lhs = lhs >= rhs;
      else if (op == "&&") lhs = lhs && rhs;
      else if (op == "||") lhs = lhs || rhs;
    }
    return lhs;
  }
};

long long EvalArith(Shell* sh, const std::string& expr) {
  Arith a(expr, sh);
  return a.expr(0);
}

// --- glob matching -------------------------------------------------------------

bool GlobMatch(const std::string& pat, const std::string& str) {
  size_t p = 0, s = 0, star_p = std::string::npos, star_s = 0;
  while (s < str.size()) {
    if (p < pat.size() && pat[p] == '\\' && p + 1 < pat.size()) {
      if (pat[p + 1] == str[s]) {
        p += 2;
        ++s;
        continue;
      }
    } else if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
      ++p;
      ++s;
      continue;
    } else if (p < pat.size() && pat[p] == '[') {
      size_t q = p + 1;
      bool neg = q < pat.size() && (pat[q] == '!' || pat[q] == '^');
      if (neg) ++q;
      bool matched = false;
      while (q < pat.size() && pat[q] != ']') {
        if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
          if (str[s] >= pat[q] && str[s] <= pat[q + 2]) matched = true;
          q += 3;
        } else {
          if (str[s] == pat[q]) matched = true;
          ++q;
        }
      }
      if (q < pat.size()) ++q;
      if (matched != neg) {
        p = q;
        ++s;
        continue;
      }
      if (star_p != std::string::npos) {
        p = star_p + 1;
        s = ++star_s;
        continue;
      }
      return false;
    } else if (p < pat.size() && pat[p] == '*') {
      star_p = p;
      star_s = s;
      ++p;
      continue;
    }
    if (star_p != std::string::npos) {
      p = star_p + 1;
      s = ++star_s;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

bool HasGlobMeta(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') {
      ++i;
      continue;
    }
    if (s[i] == '*' || s[i] == '?' || s[i] == '[') return true;
  }
  return false;
}

// --- word expansion ------------------------------------------------------------
//
// Output carries, per character, whether it may be split on IFS (came from an
// unquoted expansion) and whether it is an active glob metacharacter (a literal
// pattern character of the raw word, outside quotes).

struct Buf {
  std::string text;
  bool quoted_empty = false;
  std::string split;  // '1' where IFS splitting applies
  std::string glob;   // '1' where the char is an active pattern character
  void add(const std::string& s, bool sp, bool gl) {
    text += s;
    split.append(s.size(), sp ? '1' : '0');
    glob.append(s.size(), gl ? '1' : '0');
  }
  void add(char c, bool sp, bool gl) {
    text += c;
    split += sp ? '1' : '0';
    glob += gl ? '1' : '0';
  }
};

std::string ExpandParam(Shell* sh, const std::string& body, bool* did_split);
void ExpandInto(Shell* sh, const std::string& raw, bool in_dquote, Buf* out);

// Appends the value of the $-expansion at raw[*i] ('$') to `out`. Unquoted
// expansions are marked splittable; nothing from an expansion is a glob char.
void ExpandDollar(Shell* sh, const std::string& raw, size_t* i, bool in_dquote, Buf* out) {
  size_t n = raw.size();
  size_t j = *i + 1;
  bool split = !in_dquote;
  if (j >= n) {
    out->add('$', false, false);
    *i = j;
    return;
  }
  if (raw[j] == '{') {
    size_t k = j;
    ScanBalanced(raw, &k, '{', '}', nullptr);
    std::string body = raw.substr(j + 1, k - j - 2);
    bool p = false;
    std::string v = ExpandParam(sh, body, &p);
    out->add(v, split, false);
    *i = k;
    return;
  }
  if (raw[j] == '(') {
    if (j + 1 < n && raw[j + 1] == '(') {
      size_t k = j;
      ScanBalanced(raw, &k, '(', ')', nullptr);
      std::string inner = raw.substr(j + 2, (k >= 2 ? k - 2 : k) - (j + 2));
      // Expand parameters inside the arithmetic first.
      Buf eb;
      ExpandInto(sh, inner, true, &eb);
      out->add(std::to_string(EvalArith(sh, eb.text)), split, false);
      *i = k;
      return;
    }
    size_t k = j;
    ScanBalanced(raw, &k, '(', ')', nullptr);
    std::string cmd = raw.substr(j + 1, k - j - 2);
    out->add(CommandSubstitute(sh, cmd), split, false);
    *i = k;
    return;
  }
  if (!IsNameStart(raw[j]) && !IsDigitC(raw[j]) && strchr("?$!#@*-", raw[j]) == nullptr) {
    out->add('$', false, false);
    *i = j;
    return;
  }
  std::string name;
  if (IsNameStart(raw[j])) {
    size_t k = j;
    while (k < n && IsNameChar(raw[k])) name += raw[k++];
    *i = k;
  } else {
    name = std::string(1, raw[j]);
    *i = j + 1;
  }
  // "$@" in double quotes still splits into one field per positional; we
  // approximate by joining on space (fields are preserved by $@ unquoted).
  out->add(GetVar(*sh, name), split, false);
}

// Expands a ${...} body (operators :-, :=, :+, :?, #, ##, %, %%, /, //, :off:len,
// and ${#name}). Sets *did_split (unused by callers for now).
std::string ExpandParam(Shell* sh, const std::string& body, bool* did_split) {
  *did_split = false;
  if (body.empty()) return "";
  if (body[0] == '#' && body.size() > 1) {
    return std::to_string(GetVar(*sh, body.substr(1)).size());
  }
  size_t k = 0;
  if (!body.empty() && (strchr("?$!#@*-", body[0]) != nullptr || IsDigitC(body[0]))) {
    k = 1;
  } else {
    while (k < body.size() && IsNameChar(body[k])) ++k;
  }
  std::string name = body.substr(0, k);
  bool set = VarSet(*sh, name);
  std::string value = GetVar(*sh, name);
  if (k >= body.size()) return value;
  std::string rest = body.substr(k);
  auto word = [&](const std::string& w) {
    Buf b;
    ExpandInto(sh, w, true, &b);
    return b.text;
  };
  if (rest[0] == ':' && rest.size() > 1 && strchr("-=+?", rest[1]) != nullptr) {
    char op = rest[1];
    std::string arg = rest.substr(2);
    bool empty = !set || value.empty();
    if (op == '-') return empty ? word(arg) : value;
    if (op == '=') {
      if (empty) {
        std::string v = word(arg);
        SetVar(sh, name, v);
        return v;
      }
      return value;
    }
    if (op == '+') return empty ? "" : word(arg);
    if (op == '?') {
      if (empty) std::fprintf(stderr, "%s: %s\n", name.c_str(),
                              arg.empty() ? "parameter null or not set" : word(arg).c_str());
      return value;
    }
  }
  if (rest[0] == ':') {
    // ${name:offset} or ${name:offset:length}
    std::string spec = rest.substr(1);
    size_t colon = spec.find(':');
    long long off = atoll(spec.substr(0, colon).c_str());
    long long len = colon == std::string::npos ? -1 : atoll(spec.substr(colon + 1).c_str());
    long long sz = static_cast<long long>(value.size());
    if (off < 0) off = std::max<long long>(0, sz + off);
    if (off > sz) off = sz;
    long long l = len < 0 ? sz - off : std::min<long long>(len, sz - off);
    if (l < 0) l = 0;
    return value.substr(static_cast<size_t>(off), static_cast<size_t>(l));
  }
  if (rest[0] == '#') {
    bool longest = rest.size() > 1 && rest[1] == '#';
    std::string pat = word(rest.substr(longest ? 2 : 1));
    if (longest) {
      for (size_t len = value.size();; --len) {
        if (GlobMatch(pat, value.substr(0, len))) return value.substr(len);
        if (len == 0) break;
      }
    } else {
      for (size_t len = 0; len <= value.size(); ++len) {
        if (GlobMatch(pat, value.substr(0, len))) return value.substr(len);
      }
    }
    return value;
  }
  if (rest[0] == '%') {
    bool longest = rest.size() > 1 && rest[1] == '%';
    std::string pat = word(rest.substr(longest ? 2 : 1));
    if (longest) {
      for (size_t len = 0; len <= value.size(); ++len) {
        if (GlobMatch(pat, value.substr(value.size() - len))) return value.substr(0, value.size() - len);
      }
    } else {
      for (size_t len = value.size();; --len) {
        if (GlobMatch(pat, value.substr(value.size() - len))) return value.substr(0, value.size() - len);
        if (len == 0) break;
      }
    }
    return value;
  }
  if (rest[0] == '/') {
    bool all = rest.size() > 1 && rest[1] == '/';
    std::string body2 = rest.substr(all ? 2 : 1);
    size_t slash = body2.find('/');
    std::string pat = word(slash == std::string::npos ? body2 : body2.substr(0, slash));
    std::string repl = slash == std::string::npos ? "" : word(body2.substr(slash + 1));
    if (pat.empty()) return value;
    std::string out;
    size_t pos = 0;
    bool done = false;
    while (pos < value.size()) {
      bool matched = false;
      for (size_t len = value.size() - pos; len > 0 && !done; --len) {
        if (GlobMatch(pat, value.substr(pos, len))) {
          out += repl;
          pos += len;
          matched = true;
          if (!all) done = true;
          break;
        }
      }
      if (done) {
        out += value.substr(pos);
        return out;
      }
      if (!matched) {
        out += value[pos++];
      }
    }
    return out;
  }
  return value;
}

void ExpandInto(Shell* sh, const std::string& raw, bool in_dquote, Buf* out) {
  size_t i = 0, n = raw.size();
  // Tilde at the start of an unquoted word.
  if (!in_dquote && n > 0 && raw[0] == '~' && (n == 1 || raw[1] == '/')) {
    std::string home = GetVar(*sh, "HOME");
    if (home.empty()) home = "/";
    out->add(home, false, false);
    i = 1;
  }
  while (i < n) {
    char c = raw[i];
    if (!in_dquote && c == '\'') {
      out->quoted_empty = true;
      ++i;
      while (i < n && raw[i] != '\'') out->add(raw[i++], false, false);
      if (i < n) ++i;
    } else if (!in_dquote && c == '"') {
      out->quoted_empty = true;
      size_t end = i;
      ScanQuoted(raw, &end, nullptr);
      std::string inner = raw.substr(i + 1, end - i - 2);
      ExpandInto(sh, inner, true, out);
      i = end;
    } else if (c == '\\') {
      if (in_dquote) {
        if (i + 1 < n && strchr("\"\\$`", raw[i + 1]) != nullptr) {
          out->add(raw[++i], false, false);
          ++i;
        } else {
          out->add('\\', false, false);
          ++i;
        }
      } else {
        if (i + 1 < n) out->add(raw[++i], false, false);
        ++i;
      }
    } else if (c == '$') {
      ExpandDollar(sh, raw, &i, in_dquote, out);
    } else if (c == '`') {
      size_t k = i + 1;
      std::string cmd;
      while (k < n && raw[k] != '`') {
        if (raw[k] == '\\' && k + 1 < n && strchr("$`\\", raw[k + 1]) != nullptr) ++k;
        cmd += raw[k++];
      }
      out->add(CommandSubstitute(sh, cmd), !in_dquote, false);
      i = (k < n ? k + 1 : k);
    } else if (!in_dquote && (c == '*' || c == '?' || c == '[')) {
      out->add(c, false, true);
      ++i;
    } else {
      out->add(c, false, false);
      ++i;
    }
  }
}

// Pathname expansion of one field (text with glob flags). Returns matches, or
// the field itself if there are none (or no active metacharacter).
void GlobExpand(Shell* sh, const std::string& text, const std::string& glob,
                std::vector<std::string>* out) {
  bool active = false;
  for (size_t i = 0; i < glob.size(); ++i) {
    if (glob[i] == '1' && (text[i] == '*' || text[i] == '?' || text[i] == '[')) active = true;
  }
  if (sh->opt_f || !active) {
    out->push_back(text);
    return;
  }
  // Split into path components.
  std::vector<std::string> comps;
  std::string cur;
  bool absolute = !text.empty() && (text[0] == '/' || text[0] == '\\');
  for (char c : text) {
    if (c == '/') {
      comps.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  comps.push_back(cur);
  std::vector<std::string> paths;
  paths.push_back(absolute ? std::string(1, '/') : std::string());
  for (size_t ci = 0; ci < comps.size(); ++ci) {
    const std::string& comp = comps[ci];
    bool last = ci + 1 == comps.size();
    if (comp.empty()) continue;
    std::vector<std::string> next;
    for (const std::string& base : paths) {
      if (!HasGlobMeta(comp)) {
        std::string joined = base.empty() ? comp : (base.back() == '/' ? base + comp : base + "/" + comp);
        next.push_back(joined);
        continue;
      }
      std::string dir = base.empty() ? "." : base;
      std::wstring wdir = WindowsPath(dir, sh->cwd);
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((wdir + L"\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          std::string name = Narrow(fd.cFileName);
          if (name == "." || name == "..") continue;
          if (!name.empty() && name[0] == '.' && (comp.empty() || comp[0] != '.')) continue;
          if (GlobMatch(comp, name)) {
            if (!last && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            std::string joined = base.empty() ? name : (base.back() == '/' ? base + name : base + "/" + name);
            next.push_back(joined);
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
    }
    paths = next;
  }
  if (paths.empty()) {
    out->push_back(text);
  } else {
    std::sort(paths.begin(), paths.end());
    for (const std::string& p : paths) out->push_back(p);
  }
}

// Expands one raw word into fields (splitting and globbing applied).
void ExpandWord(Shell* sh, const std::string& raw, std::vector<std::string>* out) {
  Buf b;
  ExpandInto(sh, raw, false, &b);
  std::string ifs = Ifs(*sh);
  std::string ws, non_ws;
  for (char c : ifs) {
    if (c == ' ' || c == '\t' || c == '\n') ws += c;
    else non_ws += c;
  }
  // Field splitting on split-flagged characters.
  std::vector<std::pair<std::string, std::string>> fields;  // text, glob
  std::string cur_t, cur_g;
  bool have = false;
  auto flush = [&] {
    fields.push_back({cur_t, cur_g});
    cur_t.clear();
    cur_g.clear();
    have = false;
  };
  for (size_t i = 0; i < b.text.size(); ++i) {
    char c = b.text[i];
    bool splittable = b.split[i] == '1';
    if (splittable && ifs.find(c) != std::string::npos) {
      if (ws.find(c) != std::string::npos) {
        if (have) flush();
        // consume a run of whitespace IFS
        while (i + 1 < b.text.size() && b.split[i + 1] == '1' && ws.find(b.text[i + 1]) != std::string::npos)
          ++i;
      } else {
        // a non-whitespace IFS delimiter: always a field break
        flush();
      }
    } else {
      cur_t += c;
      cur_g += b.glob[i];
      have = true;
    }
  }
  if (have) flush();
  if (fields.empty() && !b.text.empty()) {
    // Not reached: a fully-unsplit word produces one field below.
  }
  if (fields.empty()) {
    // An unsplit word, or an all-empty split: keep one field only if there was
    // literal/quoted content.
    bool any_unsplit = false;
    for (char s : b.split) {
      if (s == '0') any_unsplit = true;
    }
    if (any_unsplit || b.quoted_empty) fields.push_back({b.text, b.glob});
  }
  for (auto& f : fields) GlobExpand(sh, f.first, f.second, out);
}

// Expands one raw word into one string (no splitting; for assignments,
// redirect targets, case words, for-less loops).
std::string ExpandOne(Shell* sh, const std::string& raw) {
  Buf b;
  ExpandInto(sh, raw, false, &b);
  return b.text;
}

}  // namespace

// Defined after the parser and executor; see the bottom of the file.
int RunShell(const std::vector<std::string>& argv);

}  // namespace gesher::exec
