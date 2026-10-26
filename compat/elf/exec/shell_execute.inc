// Included inside gesher_shell.cc's anonymous namespace. Shell state belongs
// to a shell process; pipelines and subshells use the same launcher and fd
// inheritance as Android executables, without changing the parent's cwd/env.

std::string Quote(const std::string& text) {
  std::string out = "'";
  for (char c : text) out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
}

std::string Source(const NodeP& n) {
  if (!n) return ":";
  std::string out;
  switch (n->kind) {
    case Node::Simple:
      for (const auto& [name, value] : n->assigns)
        out += name + "=" + value + " ";
      for (const auto& word : n->words) out += word + " ";
      if (out.empty()) out = ":";
      break;
    case Node::Pipeline:
    case Node::AndOr:
    case Node::List:
      if (n->negate) out = "! ";
      for (size_t i = 0; i < n->children.size(); ++i) {
        if (i > 0) {
          if (n->kind == Node::Pipeline)
            out += " | ";
          else if (n->kind == Node::AndOr)
            out += " " + n->connectors[i - 1] + " ";
          else
            out += i - 1 < n->terminators.size() && n->terminators[i - 1] == "&"
                       ? " &\n"
                       : "\n";
        }
        out += Source(n->children[i]);
      }
      if (n->kind == Node::List && !n->terminators.empty() &&
          n->terminators.back() == "&")
        out += " &";
      break;
    case Node::If:
      out = "if\n" + Source(n->cond) + "\nthen\n" + Source(n->body);
      for (size_t i = 0; i + 1 < n->elifs.size(); i += 2) {
        out += "\nelif\n" + Source(n->elifs[i]) + "\nthen\n" +
               Source(n->elifs[i + 1]);
      }
      if (n->else_body) out += "\nelse\n" + Source(n->else_body);
      out += "\nfi";
      break;
    case Node::For:
      out = "for " + n->var;
      if (n->for_in) {
        out += " in";
        for (const auto& word : n->for_words) out += " " + word;
      }
      out += "\ndo\n" + Source(n->body) + "\ndone";
      break;
    case Node::While:
      out = std::string(n->until ? "until\n" : "while\n") + Source(n->cond) +
            "\ndo\n" + Source(n->body) + "\ndone";
      break;
    case Node::Case:
      out = "case " + n->case_word + " in\n";
      for (const auto& [patterns, body] : n->cases) {
        for (size_t i = 0; i < patterns.size(); ++i)
          out += (i ? "|" : "") + patterns[i];
        out += ")\n" + Source(body) + "\n;;\n";
      }
      out += "esac";
      break;
    case Node::Group:
      out = "{\n" + Source(n->body) + "\n}";
      break;
    case Node::Subshell:
      out = "(\n" + Source(n->body) + "\n)";
      break;
    case Node::FuncDef:
      out = n->func_name + "() " + Source(n->body);
      break;
    case Node::Arith:
      out = "let " + (n->words.empty() ? "0" : Quote(n->words[0]));
      break;
  }
  std::string documents;
  for (const Redir& r : n->redirs) {
    out += " " + std::to_string(r.fd) + r.op;
    if (r.op == "<<") {
      std::string delimiter = "GESHER_HERE_DOCUMENT";
      while (r.target.find(delimiter) != std::string::npos) delimiter += "_";
      out += r.heredoc_expand ? delimiter : Quote(delimiter);
      documents += "\n" + r.target;
      if (documents.back() != '\n') documents += '\n';
      documents += delimiter;
    } else {
      out += r.target;
    }
  }
  return out + documents;
}

std::string SubshellSource(const Shell& sh, const std::string& source) {
  std::string out;
  for (const auto& [name, var] : sh.vars) {
    if (!IsName(name)) continue;
    out += name + "=" + Quote(var.value) + "\n";
    out += std::string(var.exported ? "export " : "export -n ") + name + "\n";
    if (var.readonly) out += "readonly " + name + "\n";
  }
  for (const auto& [name, body] : sh.funcs)
    out += name + "() " + Source(body) + "\n";
  out += "set --";
  for (size_t i = 1; i < sh.pos.size(); ++i) out += " " + Quote(sh.pos[i]);
  out += "\n";
  if (sh.opt_u) out += "set -u\n";
  if (sh.opt_e) out += "set -e\n";
  if (sh.opt_f) out += "set -f\n";
  if (sh.opt_pipefail) out += "set -o pipefail\n";
  return out + source;
}

int Start(const Shell& sh, const std::vector<std::string>& args, const Io& io,
          PROCESS_INFORMATION* process,
          const std::vector<ChildFd>& extra = {}) {
  Launch launch;
  int error = PrepareLaunchInPath(args, sh.cwd, GetVar(sh, "PATH"), &launch);
  if (error != 0) {
    ShErr(sh, io, args[0] + ": " + (error == kENOENT ? "not found" : ErrorText(error)));
    return error == kENOENT ? 127 : 126;
  }
  std::vector<ChildFd> fds;
  for (int i = 0; i < 10; ++i) {
    if (io.fd[i] != nullptr && io.fd[i] != INVALID_HANDLE_VALUE)
      fds.push_back({i, io.fd[i]});
  }
  fds.insert(fds.end(), extra.begin(), extra.end());
  std::wstring environment = EnvironmentBlock(ChildEnv(sh));
  DWORD failed = SpawnWithFds(launch, environment.c_str(), WindowsPath(sh.cwd),
                              fds, process);
  if (failed != 0) {
    ShErr(sh, io, args[0] + ": " + ErrorText(ErrnoFromWin32(failed)));
    return 126;
  }
  CloseHandle(process->hThread);
  process->hThread = nullptr;
  return 0;
}

int WaitProcess(HANDLE process) {
  if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) return 127;
  DWORD status = 1;
  GetExitCodeProcess(process, &status);
  return ExitStatus(status);
}

HandleP Temporary(const std::string& text);

int StartSubshell(const Shell& sh, const std::string& src, const Io& io,
                  PROCESS_INFORMATION* process) {
  HandleP script = Temporary(SubshellSource(sh, src));
  if (!script) {
    ShErr(sh, io, "cannot create subshell input");
    return 1;
  }
  // The serialized state can exceed Windows' command-line limit. A private
  // inherited descriptor leaves the child's stdin available to its commands.
  return Start(sh,
               {"sh", "--gesher-script-fd", "63", std::to_string(sh.shell_pid),
                sh.pos.empty() ? "sh" : sh.pos[0]},
               io, process, {{63, script->h}});
}

int RunSubshell(Shell* sh, const std::string& src, Io io, bool background) {
  PROCESS_INFORMATION process = {};
  int failed = StartSubshell(*sh, src, io, &process);
  if (failed) return failed;
  if (background) {
    sh->jobs[process.dwProcessId] =
        std::make_shared<OwnedHandle>(process.hProcess);
    sh->last_bg = process.dwProcessId;
    return 0;
  }
  AddForeground(process.dwProcessId);
  int status = WaitProcess(process.hProcess);
  RemoveForeground(process.dwProcessId);
  CloseHandle(process.hProcess);
  return status;
}

HandleP Temporary(const std::string& text) {
  wchar_t dir[MAX_PATH], name[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"gsh", 0, name))
    return nullptr;
  HANDLE handle = CreateFileW(
      name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DeleteFileW(name);
    return nullptr;
  }
  auto owned = std::make_shared<OwnedHandle>(handle);
  if (!WriteAll(handle, text)) return nullptr;
  SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
  return owned;
}

bool Redirect(Shell* sh, const std::vector<Redir>& redirs, Io* io) {
  for (const Redir& r : redirs) {
    if (r.fd < 0 || r.fd >= 10) {
      ShErr(*sh, *io, "bad file descriptor");
      return false;
    }
    if (r.op == "<<") {
      Buf body;
      if (r.heredoc_expand)
        ExpandInto(sh, r.target, true, &body);
      else
        body.text = r.target;
      HandleP file = Temporary(body.text);
      if (!file) return false;
      io->set(r.fd, file->h, file);
      continue;
    }
    std::vector<std::string> targets;
    ExpandWord(sh, r.target, &targets);
    if (targets.size() != 1) {
      ShErr(*sh, *io, "ambiguous redirect: " + r.target);
      return false;
    }
    const std::string& target = targets[0];
    if (r.op == "<&" || r.op == ">&") {
      if (target == "-")
        io->set(r.fd, nullptr);
      else if (target.size() == 1 && IsDigitC(target[0])) {
        int from = target[0] - '0';
        if (io->fd[from] == nullptr || io->fd[from] == INVALID_HANDLE_VALUE)
          return false;
        io->set(r.fd, io->fd[from], io->owned[from]);
      } else
        return false;
      continue;
    }
    bool input = r.op == "<" || r.op == "<>";
    DWORD access = input ? GENERIC_READ : GENERIC_WRITE;
    if (r.op == "<>") access |= GENERIC_WRITE;
    if (r.op == ">>") access = FILE_APPEND_DATA;
    DWORD disposition = r.op == "<"                    ? OPEN_EXISTING
                        : r.op == ">>" || r.op == "<>" ? OPEN_ALWAYS
                                                       : CREATE_ALWAYS;
    std::wstring path =
        target == "/dev/null" ? L"NUL" : WindowsPath(target, sh->cwd);
    HANDLE file =
        CreateFileW(path.c_str(), access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      ShErr(*sh, *io,
            target + ": " + ErrorText(ErrnoFromWin32(GetLastError())));
      return false;
    }
    io->set(r.fd, file, std::make_shared<OwnedHandle>(file));
  }
  return true;
}

std::string ReadAll(HANDLE input) {
  std::string text;
  char buffer[8192];
  DWORD count = 0;
  while (ReadFile(input, buffer, sizeof(buffer), &count, nullptr) && count)
    text.append(buffer, count);
  return text;
}

bool TestUnary(Shell* sh, const Io& io, const std::string& op,
               const std::string& arg) {
  if (op == "-n") return !arg.empty();
  if (op == "-z") return arg.empty();
  if (op == "-t") {
    int fd = atoi(arg.c_str());
    DWORD mode;
    return fd >= 0 && fd < 10 && GetConsoleMode(io.fd[fd], &mode);
  }
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(WindowsPath(arg, sh->cwd).c_str(),
                            GetFileExInfoStandard, &data))
    return false;
  if (op == "-e" || op == "-r" || op == "-x") return true;
  if (op == "-d")
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (op == "-f")
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  if (op == "-L" || op == "-h")
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  if (op == "-w") return (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
  if (op == "-s") return data.nFileSizeHigh != 0 || data.nFileSizeLow != 0;
  return false;
}

struct TestExpression {
  Shell* sh;
  const Io& io;
  const std::vector<std::string>& args;
  size_t at = 1, end;
  bool ok = true;
  bool primary() {
    if (at >= end) {
      ok = false;
      return false;
    }
    std::string left = args[at++];
    if (left == "!") return !primary();
    if (left == "(") {
      bool value = expression();
      if (at >= end || args[at++] != ")") ok = false;
      return value;
    }
    if (at < end && left.size() == 2 && left[0] == '-' &&
        std::string("nzterxdfLhws").find(left[1]) != std::string::npos) {
      return TestUnary(sh, io, left, args[at++]);
    }
    if (at + 1 < end && args[at] != "-a" && args[at] != "-o" &&
        args[at] != ")") {
      std::string op = args[at++], right = args[at++];
      if (op == "=" || op == "==") return left == right;
      if (op == "!=") return left != right;
      if (op == "<") return left < right;
      if (op == ">") return left > right;
      char *a = nullptr, *b = nullptr;
      long long x = strtoll(left.c_str(), &a, 10),
                y = strtoll(right.c_str(), &b, 10);
      if (left.empty() || right.empty() || *a || *b) {
        ok = false;
        return false;
      }
      if (op == "-eq") return x == y;
      if (op == "-ne") return x != y;
      if (op == "-lt") return x < y;
      if (op == "-le") return x <= y;
      if (op == "-gt") return x > y;
      if (op == "-ge") return x >= y;
      ok = false;
      return false;
    }
    return !left.empty();
  }
  bool conjunction() {
    bool value = primary();
    while (at < end && args[at] == "-a") {
      ++at;
      bool next = primary();
      value = value && next;
    }
    return value;
  }
  bool expression() {
    bool value = conjunction();
    while (at < end && args[at] == "-o") {
      ++at;
      bool next = conjunction();
      value = value || next;
    }
    return value;
  }
};

std::string Escapes(const std::string& text, bool* stop) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 == text.size()) {
      out += text[i];
      continue;
    }
    char c = text[++i];
    switch (c) {
      case 'a':
        out += '\a';
        break;
      case 'b':
        out += '\b';
        break;
      case 'c':
        *stop = true;
        return out;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'v':
        out += '\v';
        break;
      case '\\':
        out += '\\';
        break;
      case '0': {
        unsigned value = 0;
        for (int n = 0; n < 3 && i + 1 < text.size() && text[i + 1] >= '0' &&
                        text[i + 1] <= '7';
             ++n) {
          value = value * 8 + text[++i] - '0';
        }
        out += static_cast<char>(value);
        break;
      }
      default:
        out += '\\';
        out += c;
        break;
    }
  }
  return out;
}

int Printf(const std::vector<std::string>& args, const Io& io) {
  if (args.size() < 2) return 0;
  size_t argument = 2;
  bool stop = false;
  do {
    size_t before = argument;
    std::string out;
    const std::string& format = args[1];
    for (size_t i = 0; i < format.size() && !stop; ++i) {
      if (format[i] == '\\') {
        size_t end = i + 2;
        if (end <= format.size() && format[i + 1] == '0') {
          while (end < format.size() && end < i + 5 && format[end] >= '0' &&
                 format[end] <= '7')
            ++end;
        }
        out += Escapes(format.substr(i, end - i), &stop);
        i = end - 1;
        continue;
      }
      if (format[i] != '%') {
        out += format[i];
        continue;
      }
      if (++i == format.size()) return 2;
      if (format[i] == '%') {
        out += '%';
        continue;
      }
      std::string spec = "%";
      while (i < format.size() &&
             (strchr("-+ #0.123456789", format[i]) != nullptr))
        spec += format[i++];
      if (i == format.size()) return 2;
      char conversion = format[i];
      std::string value = argument < args.size() ? args[argument++] : "";
      char buffer[4096];
      int count = 0;
      if (conversion == 'b') {
        out += Escapes(value, &stop);
        continue;
      }
      if (conversion == 's')
        count = snprintf(buffer, sizeof(buffer), (spec + "s").c_str(),
                         value.c_str());
      else if (conversion == 'c')
        count = snprintf(buffer, sizeof(buffer), (spec + "c").c_str(),
                         value.empty() ? 0 : value[0]);
      else if (strchr("diouxX", conversion)) {
        long long number =
            !value.empty() && (value[0] == '\'' || value[0] == '"')
                ? (value.size() > 1 ? static_cast<unsigned char>(value[1]) : 0)
                : strtoll(value.c_str(), nullptr, 0);
        if (conversion == 'd' || conversion == 'i')
          count = snprintf(buffer, sizeof(buffer),
                           (spec + "ll" + conversion).c_str(), number);
        else
          count = snprintf(buffer, sizeof(buffer),
                           (spec + "ll" + conversion).c_str(),
                           static_cast<unsigned long long>(number));
      } else
        return 2;
      if (count < 0 || static_cast<size_t>(count) >= sizeof(buffer)) return 2;
      out.append(buffer, static_cast<size_t>(count));
    }
    if (!WriteAll(io.fd[1], out)) return 1;
    if (stop || argument == before) break;
  } while (argument < args.size());
  return 0;
}

bool IsBuiltin(const std::string& name) {
  static const char* names[] = {
      ":",       "true",     "false",  "echo",  "printf",   "pwd",   "cd",
      "export",  "readonly", "unset",  "set",   "shift",    "read",  "test",
      "[",       "exit",     "return", "break", "continue", "local", ".",
      "source",  "eval",     "let",    "trap",  "wait",     "jobs",  "kill",
      "command", "type",     "exec",   "umask", "getopts"};
  for (const char* builtin : names)
    if (name == builtin) return true;
  return false;
}

bool IsSpecialBuiltin(const std::string& name) {
  static const char* names[] = {":", ".", "source", "break", "continue", "eval",
                                "exec", "exit", "export", "readonly", "return",
                                "set", "shift", "trap", "unset"};
  for (const char* builtin : names)
    if (name == builtin) return true;
  return false;
}

int Builtin(Shell* sh, const std::vector<std::string>& args, Io* io);
int Simple(Shell* sh, const NodeP& node, Io io, bool* retain_redirs);
void RunPendingSignals(Shell* sh, Io io);

int ExecNode(Shell* sh, const NodeP& node, Io io) {
  if (!node || sh->exiting || sh->returning || sh->breaking || sh->continuing)
    return sh->status;
  if (node->kind == Node::List && node->children.empty()) return sh->status;
  for (const auto& [fd, persistent] : sh->persistent_fd) {
    if (io.persistent_generation[fd] != persistent.generation) {
      io.set(fd, persistent.handle->h, persistent.handle);
      io.persistent_generation[fd] = persistent.generation;
    }
  }
  // An enclosing temporary redirection is restored after its command even
  // when a nested exec changes that same descriptor. Other exec descriptors
  // continue to affect the shell after the enclosing command completes.
  std::map<int, std::optional<Shell::PersistentFd>> scoped_fds;
  for (const auto& redir : node->redirs) {
    auto found = sh->persistent_fd.find(redir.fd);
    scoped_fds.emplace(redir.fd, found == sh->persistent_fd.end()
                                    ? std::nullopt
                                    : std::optional<Shell::PersistentFd>(found->second));
  }
  Io saved = sh->io;
  sh->io = io;
  int status = 0;
  bool retain_redirs = false;
  bool redirected = Redirect(sh, node->redirs, &io);
  sh->io = io;
  if (!redirected)
    status = 1;
  else
    switch (node->kind) {
      case Node::Simple:
        status = Simple(sh, node, io, &retain_redirs);
        break;
      case Node::Group:
        status = ExecNode(sh, node->body, io);
        break;
      case Node::Subshell:
        status = RunSubshell(sh, Source(node->body), io);
        break;
      case Node::FuncDef:
        sh->funcs[node->func_name] = node->body;
        break;
      case Node::Arith:
        status = EvalArith(sh, node->words.empty() ? "0" : node->words[0]) == 0;
        break;
      case Node::List:
        for (size_t i = 0; i < node->children.size(); ++i) {
          bool background =
              i < node->terminators.size() && node->terminators[i] == "&";
          status = background
                       ? RunSubshell(sh, Source(node->children[i]), io, true)
                       : ExecNode(sh, node->children[i], io);
          sh->status = status;
          if (sh->exiting || sh->returning || sh->breaking || sh->continuing)
            break;
        }
        break;
      case Node::AndOr:
        for (size_t i = 0; i < node->children.size(); ++i) {
          if (i && ((node->connectors[i - 1] == "&&" && status) ||
                    (node->connectors[i - 1] == "||" && !status)))
            continue;
          bool condition = i + 1 < node->children.size();
          if (condition) ++sh->cond_depth;
          status = ExecNode(sh, node->children[i], io);
          if (condition) --sh->cond_depth;
          if (sh->exiting || sh->returning || sh->breaking || sh->continuing)
            break;
        }
        break;
      case Node::If: {
        ++sh->cond_depth;
        bool take = ExecNode(sh, node->cond, io) == 0;
        --sh->cond_depth;
        if (take)
          status = ExecNode(sh, node->body, io);
        else {
          bool selected = false;
          for (size_t i = 0; i + 1 < node->elifs.size(); i += 2) {
            ++sh->cond_depth;
            take = ExecNode(sh, node->elifs[i], io) == 0;
            --sh->cond_depth;
            if (take) {
              status = ExecNode(sh, node->elifs[i + 1], io);
              selected = true;
              break;
            }
          }
          if (!selected && node->else_body)
            status = ExecNode(sh, node->else_body, io);
        }
        break;
      }
      case Node::For:
      case Node::While: {
        std::vector<std::string> values;
        if (node->kind == Node::For) {
          if (node->for_in)
            for (const auto& word : node->for_words)
              ExpandWord(sh, word, &values);
          else if (sh->pos.size() > 1)
            values.assign(sh->pos.begin() + 1, sh->pos.end());
        }
        ++sh->loop_depth;
        for (size_t i = 0;; ++i) {
          if (node->kind == Node::For) {
            if (i >= values.size()) break;
            SetVar(sh, node->var, values[i]);
          } else {
            ++sh->cond_depth;
            bool condition = ExecNode(sh, node->cond, io) == 0;
            --sh->cond_depth;
            if (condition == node->until) break;
          }
          status = ExecNode(sh, node->body, io);
          if (sh->exiting || sh->returning) break;
          if (sh->breaking) {
            --sh->breaking;
            break;
          }
          if (sh->continuing) {
            if (--sh->continuing) break;
          }
        }
        --sh->loop_depth;
        break;
      }
      case Node::Case: {
        std::string value = ExpandOne(sh, node->case_word);
        bool found = false;
        for (const auto& [patterns, body] : node->cases) {
          for (const auto& pattern : patterns) {
            if (GlobMatch(ExpandOne(sh, pattern), value)) {
              status = ExecNode(sh, body, io);
              found = true;
              break;
            }
          }
          if (found) break;
        }
        break;
      }
      case Node::Pipeline: {
        std::vector<PROCESS_INFORMATION> processes;
        HandleP previous;
        for (size_t i = 0; i < node->children.size(); ++i) {
          Io stage = io;
          if (previous) stage.set(0, previous->h, previous);
          HandleP reader, writer;
          if (i + 1 < node->children.size()) {
            HANDLE read_end, write_end;
            if (!CreatePipe(&read_end, &write_end, nullptr, 0)) {
              status = 1;
              break;
            }
            reader = std::make_shared<OwnedHandle>(read_end);
            writer = std::make_shared<OwnedHandle>(write_end);
            stage.set(1, writer->h, writer);
          }
          PROCESS_INFORMATION child = {};
          status = StartSubshell(*sh, Source(node->children[i]), stage, &child);
          if (status) break;
          AddForeground(child.dwProcessId);
          processes.push_back(child);
          previous = reader;
        }
        previous.reset();
        int rightmost_failure = status;
        int launch_failure = status;
        for (auto& child : processes) {
          int code = WaitProcess(child.hProcess);
          RemoveForeground(child.dwProcessId);
          CloseHandle(child.hProcess);
          if (code) rightmost_failure = code;
          status = code;
        }
        if (sh->opt_pipefail && rightmost_failure) status = rightmost_failure;
        if (launch_failure) status = launch_failure;
        if (node->negate) status = !status;
        break;
      }
    }
  if (!retain_redirs) {
    for (const auto& [fd, before] : scoped_fds) {
      auto current = sh->persistent_fd.find(fd);
      if (before) {
        if (current == sh->persistent_fd.end() || current->second.generation != before->generation)
          sh->persistent_fd[fd] = {before->handle, ++sh->fd_generation};
      } else {
        sh->persistent_fd.erase(fd);
      }
    }
  }
  sh->io = saved;
  sh->status = status;
  RunPendingSignals(sh, io);
  status = sh->status;
  if (status && sh->opt_e && sh->cond_depth == 0 && node->kind != Node::AndOr &&
      !node->negate)
    sh->exiting = true;
  return status;
}

int RunProgramString(Shell* sh, const std::string& src, Io io) {
  auto tokens = Tokenize(src);
  Parser parser(tokens);
  NodeP program = parser.parseProgram();
  if (!parser.error.empty()) {
    ShErr(*sh, io, parser.error);
    return sh->status = 2;
  }
  return ExecNode(sh, program, io);
}

void RunPendingSignals(Shell* sh, Io io) {
  for (int word = 0; word < 2; ++word) {
    uint64_t pending = g_pending_signals[word].exchange(0);
    for (int bit = 0; bit < 64; ++bit) {
      if (!(pending & (uint64_t{1} << bit))) continue;
      int signal = word * 64 + bit;
      auto trap = sh->traps.find(signal);
      if (trap == sh->traps.end()) continue;
      std::string action = trap->second;
      int status = sh->status;
      RunProgramString(sh, action, io);
      if (!sh->exiting) sh->status = status;
    }
  }
}
