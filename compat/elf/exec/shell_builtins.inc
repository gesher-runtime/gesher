// Included by gesher_shell.cc after the executor.

int SetOptions(Shell* sh, const std::vector<std::string>& args, size_t first,
               const Io& io) {
  size_t i = first;
  for (; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--") {
      ++i;
      break;
    }
    if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '+')) break;
    bool value = arg[0] == '-';
    for (size_t j = 1; j < arg.size(); ++j) {
      switch (arg[j]) {
        case 'e':
          sh->opt_e = value;
          break;
        case 'u':
          sh->opt_u = value;
          break;
        case 'x':
          sh->opt_x = value;
          break;
        case 'f':
          sh->opt_f = value;
          break;
        case 'o':
          if (++i == args.size() || args[i] != "pipefail") {
            ShErr(*sh, io, "set: expected pipefail after -o/+o");
            return -1;
          }
          sh->opt_pipefail = value;
          break;
        default:
          ShErr(*sh, io, "set: unsupported option " + arg);
          return -1;
      }
    }
  }
  return static_cast<int>(i);
}

int External(Shell* sh, const std::vector<std::string>& args, const Io& io) {
  PROCESS_INFORMATION process = {};
  int failed = Start(*sh, args, io, &process);
  if (failed) return failed;
  AddForeground(process.dwProcessId);
  int status = WaitProcess(process.hProcess);
  RemoveForeground(process.dwProcessId);
  CloseHandle(process.hProcess);
  return status;
}

int Builtin(Shell* sh, const std::vector<std::string>& args, Io* io) {
  const std::string& name = args[0];
  auto fail = [&](const std::string& message) {
    ShErr(*sh, *io, name + ": " + message);
    return 2;
  };
  if (name == ":" || name == "true") return 0;
  if (name == "false") return 1;
  if (name == "printf") return Printf(args, *io);
  if (name == "echo") {
    size_t first = 1;
    bool newline = true, escapes = false;
    while (first < args.size() && args[first].size() > 1 &&
           args[first][0] == '-' &&
           args[first].find_first_not_of("neE", 1) == std::string::npos) {
      for (size_t i = 1; i < args[first].size(); ++i) {
        if (args[first][i] == 'n')
          newline = false;
        else
          escapes = args[first][i] == 'e';
      }
      ++first;
    }
    std::string text;
    bool stop = false;
    for (size_t i = first; i < args.size() && !stop; ++i) {
      if (i > first) text += ' ';
      text += escapes ? Escapes(args[i], &stop) : args[i];
    }
    if (newline && !stop) text += '\n';
    return WriteAll(io->fd[1], text) ? 0 : 1;
  }
  if (name == "pwd") return WriteAll(io->fd[1], sh->cwd + "\n") ? 0 : 1;
  if (name == "cd") {
    size_t at = 1;
    if (at < args.size() &&
        (args[at] == "-L" || args[at] == "-P" || args[at] == "--"))
      ++at;
    if (args.size() > at + 1) return fail("too many arguments");
    std::string target = at < args.size() ? args[at] : GetVar(*sh, "HOME");
    bool print = target == "-";
    if (print) target = GetVar(*sh, "OLDPWD");
    if (target.empty()) return fail("directory is not set");
    std::wstring path = WindowsPath(target, sh->cwd);
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      ShErr(*sh, *io, "cd: " + target + ": not a directory");
      return 1;
    }
    SetVar(sh, "OLDPWD", sh->cwd);
    sh->cwd = Narrow(path);
    SetVar(sh, "PWD", sh->cwd);
    if (print) WriteAll(io->fd[1], sh->cwd + "\n");
    return 0;
  }
  if (name == "export" || name == "readonly" || name == "local") {
    bool unexport = args.size() > 1 && args[1] == "-n";
    size_t first = unexport || (args.size() > 1 && args[1] == "-p") ? 2 : 1;
    if (name == "local" && sh->local_scopes.empty())
      return fail("outside a function");
    if (args.size() <= first && name != "local") {
      for (const auto& [key, var] : sh->vars) {
        if ((name == "export" && var.exported) ||
            (name == "readonly" && var.readonly)) {
          WriteAll(io->fd[1], name + " " + key + "=" + Quote(var.value) + "\n");
        }
      }
      return 0;
    }
    for (size_t i = first; i < args.size(); ++i) {
      size_t equal = args[i].find('=');
      std::string key = args[i].substr(0, equal);
      if (!IsName(key)) return fail("invalid name: " + key);
      auto found = sh->vars.find(key);
      if (found != sh->vars.end() && found->second.readonly &&
          equal != std::string::npos)
        return fail("readonly variable: " + key);
      if (name == "local") {
        auto& scope = sh->local_scopes.back();
        bool saved =
            std::any_of(scope.begin(), scope.end(),
                        [&](const auto& entry) { return entry.first == key; });
        if (!saved)
          scope.push_back({key, found == sh->vars.end()
                                    ? std::nullopt
                                    : std::optional<Var>(found->second)});
      }
      if (equal != std::string::npos)
        SetVar(sh, key, args[i].substr(equal + 1));
      if (name == "export") sh->vars[key].exported = !unexport;
      if (name == "readonly") sh->vars[key].readonly = true;
    }
    return 0;
  }
  if (name == "unset") {
    bool functions = args.size() > 1 && args[1] == "-f";
    size_t first = functions || (args.size() > 1 && args[1] == "-v") ? 2 : 1;
    for (size_t i = first; i < args.size(); ++i) {
      if (functions)
        sh->funcs.erase(args[i]);
      else {
        auto found = sh->vars.find(args[i]);
        if (found != sh->vars.end() && found->second.readonly)
          return fail("readonly variable: " + args[i]);
        sh->vars.erase(args[i]);
      }
    }
    return 0;
  }
  if (name == "set") {
    if (args.size() == 1) {
      for (const auto& [key, var] : sh->vars)
        WriteAll(io->fd[1], key + "=" + Quote(var.value) + "\n");
      return 0;
    }
    int next = SetOptions(sh, args, 1, *io);
    if (next < 0) return 2;
    if (static_cast<size_t>(next) < args.size() || args.back() == "--") {
      sh->pos.resize(1);
      sh->pos.insert(sh->pos.end(), args.begin() + next, args.end());
    }
    return 0;
  }
  if (name == "shift") {
    if (args.size() > 2 || (args.size() == 2 && !IsNumber(args[1])))
      return fail("expected a count");
    size_t count = args.size() == 2 ? strtoul(args[1].c_str(), nullptr, 10) : 1;
    if (count >= sh->pos.size()) return 1;
    sh->pos.erase(sh->pos.begin() + 1, sh->pos.begin() + 1 + count);
    return 0;
  }
  if (name == "exit" || name == "return") {
    if (args.size() > 2) return fail("too many arguments");
    int status = sh->status;
    if (args.size() > 1) {
      char* end = nullptr;
      long value = strtol(args[1].c_str(), &end, 10);
      if (args[1].empty() || *end) return fail("numeric status required");
      status = static_cast<unsigned char>(value);
    }
    if (name == "exit")
      sh->exiting = true;
    else if (sh->func_depth)
      sh->returning = true;
    else
      return fail("outside a function or sourced file");
    return status;
  }
  if (name == "break" || name == "continue") {
    if (!sh->loop_depth) return fail("outside a loop");
    int count =
        args.size() > 1 && IsNumber(args[1]) ? atoi(args[1].c_str()) : 1;
    if (args.size() > 2 || count < 1 || (args.size() > 1 && !IsNumber(args[1])))
      return fail("invalid loop count");
    (name == "break" ? sh->breaking : sh->continuing) =
        std::min(count, sh->loop_depth);
    return 0;
  }
  if (name == "test" || name == "[") {
    size_t end = args.size();
    if (name == "[") {
      if (end < 2 || args.back() != "]") return fail("missing ]");
      --end;
    }
    if (end == 1) return 1;
    TestExpression expression{sh, *io, args, 1, end};
    bool value = expression.expression();
    return expression.ok && expression.at == end ? !value
                                                 : fail("invalid expression");
  }
  if (name == "eval") {
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
      if (i > 1) text += ' ';
      text += args[i];
    }
    return RunProgramString(sh, text, *io);
  }
  if (name == "let") {
    long long value = 0;
    for (size_t i = 1; i < args.size(); ++i) value = EvalArith(sh, args[i]);
    return value == 0;
  }
  if (name == "." || name == "source") {
    if (args.size() < 2) return fail("filename required");
    std::wstring path = WindowsPath(args[1], sh->cwd);
    if (args[1].find_first_of("/\\:") == std::string::npos) {
      std::wstring found = FindInPath(args[1], GetVar(*sh, "PATH"));
      if (!found.empty()) path = found;
    }
    HandleP file = std::make_shared<OwnedHandle>(
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr));
    if (file->h == INVALID_HANDLE_VALUE) return fail("cannot open " + args[1]);
    auto saved = sh->pos;
    if (args.size() > 2) {
      sh->pos.resize(1);
      sh->pos.insert(sh->pos.end(), args.begin() + 2, args.end());
    }
    ++sh->func_depth;
    int status = RunProgramString(sh, ReadAll(file->h), *io);
    --sh->func_depth;
    sh->returning = false;
    if (args.size() > 2) sh->pos = std::move(saved);
    return status;
  }
  if (name == "read") {
    bool raw = args.size() > 1 && args[1] == "-r";
    size_t first = raw ? 2 : 1;
    std::vector<std::string> names(args.begin() + first, args.end());
    if (names.empty()) names.push_back("REPLY");
    for (const auto& key : names)
      if (!IsName(key)) return fail("invalid name: " + key);
    std::string line;
    bool escaped = false, eof = false;
    for (;;) {
      char c;
      DWORD count = 0;
      if (!ReadFile(io->fd[0], &c, 1, &count, nullptr) || !count) {
        eof = true;
        break;
      }
      if (!raw && !escaped && c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '\n' && !escaped) break;
      if (escaped && c == '\n') {
        escaped = false;
        continue;
      }
      line += c;
      escaped = false;
    }
    std::string separators = Ifs(*sh);
    size_t at = 0;
    for (size_t i = 0; i < names.size(); ++i) {
      if (args.size() == first) {
        SetVar(sh, names[i], line);
        break;
      }
      while (at < line.size() && separators.find(line[at]) != std::string::npos)
        ++at;
      size_t end = i + 1 == names.size() ? line.find_last_not_of(" \t") + 1
                                         : line.find_first_of(separators, at);
      if (end == std::string::npos || end < at) end = line.size();
      SetVar(sh, names[i], line.substr(at, end - at));
      at = end;
    }
    return eof ? 1 : 0;
  }
  if (name == "trap") {
    if (args.size() == 1 || (args.size() == 2 && args[1] == "-p")) {
      for (const auto& [signal, action] : sh->traps)
        WriteAll(io->fd[1], "trap -- " + Quote(action) + " " +
                                std::to_string(signal) + "\n");
      return 0;
    }
    size_t at = args[1] == "--" ? 2 : 1;
    if (at + 1 >= args.size()) return fail("action and signal required");
    std::string action = args[at++];
    for (; at < args.size(); ++at) {
      int signal = SignalNumber(args[at]);
      if (signal < 0 || signal == 9 || signal == 19)
        return fail("invalid or uncatchable signal: " + args[at]);
      if (action == "-")
        sh->traps.erase(signal);
      else
        sh->traps[signal] = action;
      g_trap_mode[signal].store(action == "-" ? 0 : action.empty() ? 1 : 2);
    }
    return 0;
  }
  if (name == "jobs" || name == "wait") {
    int status = 0;
    std::vector<DWORD> ids;
    if (args.size() == 1)
      for (const auto& [pid, handle] : sh->jobs) {
        (void)handle;
        ids.push_back(pid);
      }
    else
      for (size_t i = 1; i < args.size(); ++i) {
        std::string pid =
            !args[i].empty() && args[i][0] == '%' ? args[i].substr(1) : args[i];
        if (!IsNumber(pid)) return fail("invalid process: " + args[i]);
        ids.push_back(strtoul(pid.c_str(), nullptr, 10));
      }
    for (DWORD pid : ids) {
      auto found = sh->jobs.find(pid);
      if (found == sh->jobs.end()) {
        status = 127;
        continue;
      }
      if (name == "wait") {
        status = WaitProcess(found->second->h);
        sh->jobs.erase(found);
      } else
        WriteAll(io->fd[1],
                 "[" + std::to_string(pid) + "] " +
                     (WaitForSingleObject(found->second->h, 0) == WAIT_TIMEOUT
                          ? "Running\n"
                          : "Done\n"));
    }
    return args.size() == 1 && name == "wait" ? 0 : status;
  }
  if (name == "kill") {
    size_t at = 1;
    int signal = 15;
    if (at < args.size() && args[at] == "-l") {
      for (const auto& entry : kSignals)
        WriteAll(io->fd[1], std::string(entry.name) + " ");
      WriteAll(io->fd[1], "\n");
      return 0;
    }
    if (at < args.size() && args[at] == "-s") {
      if (++at == args.size()) return fail("signal required");
      signal = SignalNumber(args[at++]);
    } else if (at < args.size() && args[at].size() > 1 && args[at][0] == '-')
      signal = SignalNumber(args[at++].substr(1));
    if (signal < 0 || at == args.size())
      return fail("signal and process required");
    int status = 0;
    for (; at < args.size(); ++at) {
      if (!IsNumber(args[at]) ||
          gesher_kill_other(atoi(args[at].c_str()), signal) != 0)
        status = 1;
    }
    return status;
  }
  if (name == "command" || name == "type") {
    size_t first = 1;
    bool query = name == "type";
    if (first < args.size() && (args[first] == "-v" || args[first] == "-V")) {
      query = true;
      ++first;
    }
    if (first < args.size() && args[first] == "--") ++first;
    if (first == args.size()) return 0;
    if (!query) {
      std::vector<std::string> command(args.begin() + first, args.end());
      return IsBuiltin(command[0]) ? Builtin(sh, command, io)
                                   : External(sh, command, *io);
    }
    int status = 0;
    for (size_t i = first; i < args.size(); ++i) {
      bool builtin = IsBuiltin(args[i]) || sh->funcs.count(args[i]) ||
                     IsToolName(args[i]) || args[i] == "sh";
      std::wstring path =
          builtin ? L"" : FindInPath(args[i], GetVar(*sh, "PATH"));
      if (builtin || !path.empty())
        WriteAll(io->fd[1], (builtin ? args[i] : Narrow(path)) + "\n");
      else
        status = 1;
    }
    return status;
  }
  if (name == "exec") {
    if (args.size() == 1) return 0;  // Simple retains these redirections.
    sh->exiting = true;
    return External(sh, std::vector<std::string>(args.begin() + 1, args.end()),
                    *io);
  }
  if (name == "getopts") {
    if (args.size() < 3 || !IsName(args[2]))
      return fail("option string and variable required");
    std::vector<std::string> values =
        args.size() > 3
            ? std::vector<std::string>(args.begin() + 3, args.end())
            : std::vector<std::string>(sh->pos.begin() + 1, sh->pos.end());
    size_t index = std::max<unsigned long>(
        1, strtoul(GetVar(*sh, "OPTIND").c_str(), nullptr, 10));
    auto save_index = [&] { sh->vars["OPTIND"].value = std::to_string(index); };
    if (index > values.size() || values[index - 1].size() < 2 ||
        values[index - 1][0] != '-' || values[index - 1] == "--") {
      if (index <= values.size() && values[index - 1] == "--") ++index;
      save_index();
      SetVar(sh, args[2], "?");
      sh->getopts_offset = 1;
      return 1;
    }
    const std::string& item = values[index - 1];
    if (sh->getopts_offset >= item.size()) sh->getopts_offset = 1;
    char option = item[sh->getopts_offset++];
    size_t found = args[1].find(option);
    bool silent = !args[1].empty() && args[1][0] == ':';
    sh->vars.erase("OPTARG");
    if (option == ':' || found == std::string::npos) {
      SetVar(sh, args[2], "?");
      if (silent)
        SetVar(sh, "OPTARG", std::string(1, option));
      else
        ShErr(*sh, *io, "getopts: unknown option -" + std::string(1, option));
    } else if (found + 1 < args[1].size() && args[1][found + 1] == ':') {
      SetVar(sh, args[2], std::string(1, option));
      if (sh->getopts_offset < item.size())
        SetVar(sh, "OPTARG", item.substr(sh->getopts_offset));
      else if (index < values.size())
        SetVar(sh, "OPTARG", values[index++]);
      else {
        SetVar(sh, args[2], silent ? ":" : "?");
        if (silent)
          SetVar(sh, "OPTARG", std::string(1, option));
        else
          ShErr(*sh, *io,
                "getopts: argument required for -" + std::string(1, option));
      }
      sh->getopts_offset = item.size();
    } else
      SetVar(sh, args[2], std::string(1, option));
    if (sh->getopts_offset >= item.size()) {
      ++index;
      sh->getopts_offset = 1;
    }
    save_index();
    return 0;
  }
  return fail("not implemented");
}

int Simple(Shell* sh, const NodeP& node, Io io, bool* retain_redirs) {
  sh->substitution_status.reset();
  std::vector<std::string> args;
  for (const auto& word : node->words) ExpandWord(sh, word, &args);
  if (sh->exiting) return sh->status;
  std::vector<std::pair<std::string, std::optional<Var>>> saved;
  for (const auto& [name, raw] : node->assigns) {
    auto found = sh->vars.find(name);
    if (found != sh->vars.end() && found->second.readonly) {
      ShErr(*sh, io, "readonly variable: " + name);
      return 2;
    }
    saved.push_back({name, found == sh->vars.end()
                               ? std::nullopt
                               : std::optional<Var>(found->second)});
    SetVar(sh, name, ExpandOne(sh, raw));
    if (sh->exiting) return sh->status;
    if (!args.empty()) ExportVar(sh, name);
  }
  if (args.empty()) return sh->substitution_status.value_or(0);
  if (sh->opt_x) {
    std::string trace = "+";
    for (const auto& arg : args) trace += " " + Quote(arg);
    Err(io, trace + "\n");
  }
  int status;
  auto function = sh->funcs.find(args[0]);
  bool special = function == sh->funcs.end() && IsSpecialBuiltin(args[0]);
  if (function != sh->funcs.end()) {
    NodeP body = function->second;
    auto positional = sh->pos;
    sh->pos = args;
    sh->pos[0] = positional[0];
    sh->local_scopes.emplace_back();
    ++sh->func_depth;
    status = ExecNode(sh, body, io);
    --sh->func_depth;
    sh->returning = false;
    for (const auto& [name, value] : sh->local_scopes.back()) {
      if (value)
        sh->vars[name] = *value;
      else
        sh->vars.erase(name);
    }
    sh->local_scopes.pop_back();
    sh->pos = std::move(positional);
  } else if (IsBuiltin(args[0]))
    status = Builtin(sh, args, &io);
  else
    status = External(sh, args, io);
  if (args[0] == "exec" && args.size() == 1) {
    *retain_redirs = true;
    for (const auto& redir : node->redirs) {
      HANDLE copy = nullptr;
      HANDLE handle = io.fd[redir.fd];
      if (handle && handle != INVALID_HANDLE_VALUE &&
          !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                           &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 1;
      sh->persistent_fd[redir.fd] = {std::make_shared<OwnedHandle>(copy), ++sh->fd_generation};
    }
  }
  // Prefixes of special builtins change the shell's own environment. Other
  // command prefixes are temporary; unwind repeated names in reverse order.
  if (!special) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      const auto& [name, value] = *it;
      if (value)
        sh->vars[name] = *value;
      else
        sh->vars.erase(name);
    }
  }
  return status;
}
