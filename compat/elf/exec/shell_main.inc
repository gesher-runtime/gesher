// Included by gesher_shell.cc outside its anonymous namespace.

namespace {
HANDLE InheritedHandle(int fd) {
  STARTUPINFOW startup = {};
  GetStartupInfoW(&startup);
  if (startup.cbReserved2 < sizeof(int) || !startup.lpReserved2 || fd < 0)
    return nullptr;
  int count = 0;
  memcpy(&count, startup.lpReserved2, sizeof(count));
  if (count <= fd || count < 0 ||
      static_cast<size_t>(count) >
          (startup.cbReserved2 - sizeof(int)) / (sizeof(intptr_t) + 1))
    return nullptr;
  if (!(startup.lpReserved2[sizeof(int) + fd] & 1)) return nullptr;
  // Probe only descriptors explicitly marked open by SpawnWithFds. Asking
  // UCRT for an arbitrary closed fd can invoke its invalid-parameter handler.
  intptr_t handle = _get_osfhandle(fd);
  return handle == -1 || handle == -2 ? nullptr
                                      : reinterpret_cast<HANDLE>(handle);
}
}  // namespace

int RunShell(const std::vector<std::string>& argv) {
  Shell sh;
  sh.io.set(0, GetStdHandle(STD_INPUT_HANDLE));
  sh.io.set(1, GetStdHandle(STD_OUTPUT_HANDLE));
  sh.io.set(2, GetStdHandle(STD_ERROR_HANDLE));
  for (int fd = 3; fd < 10; ++fd) {
    if (HANDLE handle = InheritedHandle(fd)) sh.io.set(fd, handle);
  }
  wchar_t* environment = GetEnvironmentStringsW();
  if (environment) {
    for (const wchar_t* entry = environment; *entry;
         entry += wcslen(entry) + 1) {
      std::string value = Narrow(entry);
      size_t equal = value.find('=');
      if (equal != std::string::npos && IsName(value.substr(0, equal)))
        sh.vars[value.substr(0, equal)] = {value.substr(equal + 1), true,
                                           false};
    }
    FreeEnvironmentStringsW(environment);
  }
  sh.cwd = CurrentDir();
  if (!GetVarPtr(sh, "PATH")) {
    const wchar_t* path = _wgetenv(L"PATH");
    if (path) {
      SetVar(&sh, "PATH", Narrow(path));
      ExportVar(&sh, "PATH");
    }
    sh.vars.erase("Path");
  }
  SetVar(&sh, "PWD", sh.cwd);
  ExportVar(&sh, "PWD");
  SetVar(&sh, "IFS", " \t\n");
  SetVar(&sh, "OPTIND", "1");
  sh.pos.push_back("sh");
  std::string source;
  bool command = false;
  HANDLE input = sh.io.fd[0];
  HandleP script;
  size_t at = 0;
  if (argv.size() >= 4 && argv[0] == "--gesher-script-fd") {
    int fd = atoi(argv[1].c_str());
    HANDLE handle = fd == 63 ? InheritedHandle(fd) : nullptr;
    if (!handle) return 2;
    source = ReadAll(handle);
    _close(fd);
    sh.shell_pid = strtoul(argv[2].c_str(), nullptr, 10);
    sh.pos[0] = argv[3];
    command = true;
    sh.is_subshell = true;
    at = argv.size();
  } else {
    bool stdin_mode = false;
    while (at < argv.size()) {
      const std::string& arg = argv[at];
      if (arg == "--") {
        ++at;
        break;
      }
      if (arg.size() < 2 || arg[0] != '-') break;
      if (arg == "-c") {
        if (++at == argv.size()) {
          ShErr(sh, sh.io, "-c requires a command");
          return 2;
        }
        source = argv[at++];
        command = true;
        break;
      }
      if (arg == "-s") {
        stdin_mode = true;
        ++at;
        break;
      }
      if (arg == "-o" || arg == "+o") {
        if (at + 1 == argv.size() ||
            SetOptions(&sh, {"set", arg, argv[at + 1]}, 1, sh.io) < 0)
          return 2;
        ++at;
      } else if (SetOptions(&sh, {"set", arg}, 1, sh.io) < 0)
        return 2;
      ++at;
    }
    if (command) {
      if (at < argv.size()) sh.pos[0] = argv[at++];
    } else if (!stdin_mode && at < argv.size()) {
      sh.pos[0] = argv[at++];
      HANDLE handle =
          CreateFileW(WindowsPath(sh.pos[0], sh.cwd).c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        ShErr(sh, sh.io, "cannot open " + sh.pos[0]);
        return 127;
      }
      script = std::make_shared<OwnedHandle>(handle);
      input = handle;
    }
    sh.pos.insert(sh.pos.end(), argv.begin() + at, argv.end());
  }
  sh.name = sh.pos[0];
  auto old_hook = gesher_signal_hook;
  gesher_signal_hook = ShellSignalHook;
  if (command)
    RunProgramString(&sh, source, sh.io);
  else {
    // Read only as far as the complete command. A read builtin and an external
    // child must see the following bytes on stdin, not a buffered copy here.
    std::string pending;
    for (;;) {
      char c;
      bool eof = ReadByte(&sh, sh.io, input, &c, false) == 0;
      if (eof && pending.empty()) break;
      if (!eof) {
        pending += c;
        if (c != '\n') continue;
      }
      auto tokens = Tokenize(pending);
      Parser parser(tokens);
      NodeP program = parser.parseProgram();
      if (!parser.error.empty()) {
        bool incomplete =
            parser.atEof() || parser.error == "unexpected end of input";
        if (!eof && incomplete) continue;
        ShErr(sh, sh.io, parser.error);
        sh.status = 2;
        break;
      }
      ExecNode(&sh, program, sh.io);
      pending.clear();
      if (eof || sh.exiting) break;
    }
  }
  auto exit_trap = sh.traps.find(0);
  if (exit_trap != sh.traps.end() && !exit_trap->second.empty()) {
    int status = sh.status;
    sh.exiting = false;
    RunProgramString(&sh, exit_trap->second, sh.io);
    if (!sh.exiting) sh.status = status;
  }
  gesher_signal_hook = old_hook;
  return sh.status;
}
