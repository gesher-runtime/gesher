// Internal namespace/source operations, included after owned loader state.
#pragma once
constexpr int kRtldLazy = 1, kRtldNow = 2, kRtldNoLoad = 4;
constexpr int kRtldGlobal = 0x100, kRtldNoDelete = 0x1000;
constexpr uint64_t kDf1Global = 2, kDf1NoDelete = 8;

bool HasPath(const std::string& name) { return name.find_first_of("/\\") != std::string::npos; }
std::string BaseName(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string DirectoryName(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
}
std::vector<std::string> SplitPaths(const char* value) {
  std::vector<std::string> result;
  if (value == nullptr) return result;
  std::string paths(value);
  char separator = paths.find(';') != std::string::npos ? ';' : ':';
  for (size_t begin = 0; begin < paths.size();) {
    size_t end = paths.find(separator, begin);
    if (separator == ':' && end == begin + 1 && end + 1 < paths.size() &&
        (paths[end + 1] == '/' || paths[end + 1] == '\\')) end = paths.find(':', end + 1);
    if (end == std::string::npos) end = paths.size();
    if (end != begin) result.push_back(paths.substr(begin, end - begin));
    begin = end + 1;
  }
  return result;
}
std::string CanonicalPath(const std::string& path) {
  if (path.empty()) return {};
  char* resolved = realpath(path.c_str(), nullptr);
  std::string result = resolved != nullptr ? resolved : path;
  std::free(resolved);
  std::replace(result.begin(), result.end(), '\\', '/');
  while (result.size() > 1 && result.back() == '/') result.pop_back();
  return result;
}
std::vector<std::string> CanonicalPaths(const std::vector<std::string>& paths) {
  std::vector<std::string> result;
  for (const auto& path : paths) {
    auto normalized = CanonicalPath(path);
    if (!normalized.empty()) result.push_back(std::move(normalized));
  }
  return result;
}
Namespace* DefaultNamespaceLocked() {
  static Namespace* result = [] {
    auto* ns = new Namespace();
    ns->options.name = "default";
    ns->options.isolated = false;
    ns->options.ld_library_paths = CanonicalPaths(SplitPaths(std::getenv("LD_LIBRARY_PATH")));
    ns->options.default_library_paths = {"/system/lib64", "/system_ext/lib64", "/vendor/lib64",
                                         "/odm/lib64", "/product/lib64"};
    return ns;
  }();
  return result;
}
std::vector<Namespace*>& Namespaces() {
  static std::vector<Namespace*> result{DefaultNamespaceLocked()};
  return result;
}
Namespace* g_anonymous_namespace = nullptr;
// Namespace construction policy and the VM's current application SDK have
// different lifetimes. Each newly linked group snapshots this process state.
int g_application_target_sdk_version = 36;
Library* ProcessMainHandle() {
  static Library* handle = [] {
    auto* lib = new Library();
    lib->name = "<main program>";
    lib->name_space = DefaultNamespaceLocked();
    lib->main_handle = lib->init_done = true;
    lib->rtld_flags = kRtldGlobal | kRtldNoDelete;
    return lib;
  }();
  return handle;
}
bool KnownNamespace(const Namespace* ns) {
  const auto& all = Namespaces();
  return ns != nullptr && std::find(all.begin(), all.end(), ns) != all.end();
}
bool KnownLibrary(const Library* lib) {
  return lib != nullptr && (lib == ProcessMainHandle() ||
      std::find(Loaded().begin(), Loaded().end(), lib) != Loaded().end());
}
bool Available(const Library* lib) {
  return !lib->is_closed && (!lib->provider || !lib->provider->available || lib->provider->available());
}
bool MatchesArchitecture(const Library* lib, Architecture architecture) {
  if (architecture == Architecture::kAuto) return true;
  bool arm = architecture == Architecture::kAarch64;
  return lib->provider ? (lib->provider->architecture_mask & (arm ? 2u : 1u)) != 0
                       : lib->is_aarch64 == arm;
}
Architecture LibraryArchitecture(const Library* lib) {
  return lib != nullptr && !lib->provider
      ? (lib->is_aarch64 ? Architecture::kAarch64 : Architecture::kX86_64)
      : Architecture::kAuto;
}
bool NamespaceMember(const Namespace* ns, const Library* lib) {
  return lib->name_space == ns ||
      std::find(ns->libraries.begin(), ns->libraries.end(), lib) != ns->libraries.end();
}
bool Accessible(const Namespace* ns, const Library* lib,
                const std::vector<Library*>* pending = nullptr) {
  if (ns == nullptr || lib->name_space == nullptr) return true;
  if (NamespaceMember(ns, lib)) return true;
  for (const Library* parent : lib->parents) {
    if (!parent->is_closed && parent->name_space == ns) return true;
  }
  if (pending != nullptr) {
    for (const Library* parent : *pending) {
      if (parent->name_space == ns &&
          std::find(parent->dependencies.begin(), parent->dependencies.end(), lib) !=
              parent->dependencies.end()) return true;
    }
  }
  return false;
}
bool LinkedName(const Namespace::Link& link, const std::string& soname) {
  return std::find(link.sonames.begin(), link.sonames.end(), soname) != link.sonames.end();
}
bool PathAccessible(const Namespace* ns, const std::string& file) {
  if (!ns->options.isolated) return true;
  const std::string directory = DirectoryName(file);
  for (const auto* paths : {&ns->options.ld_library_paths, &ns->options.default_library_paths}) {
    for (const auto& path : *paths) if (directory == CanonicalPath(path)) return true;
  }
  for (const auto& path : ns->options.permitted_paths) {
    const std::string root = CanonicalPath(path);
    if (!root.empty() && (file == root ||
        (file.size() > root.size() && file.compare(0, root.size(), root) == 0 &&
         (root == "/" || file[root.size()] == '/')))) return true;
  }
  return false;
}
bool ContainsAddress(const Library* lib, const void* address) {
  if (lib->is_closed || lib->provider || lib->phdr == nullptr) return false;
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  for (size_t i = 0; i < lib->phnum; ++i) {
    const Elf64_Phdr& segment = lib->phdr[i];
    if (segment.p_type != PT_LOAD) continue;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(lib->load_bias) + segment.p_vaddr;
    if (value >= begin && value - begin < segment.p_memsz) return true;
  }
  return false;
}
Library* FindCallerLocked(const void* address) {
  if (address == nullptr) return nullptr;
  for (Library* lib : Loaded()) if (ContainsAddress(lib, address)) return lib;
  // An IFUNC may call libdl while its graph is private to this thread.
  for (const auto* scope : g_pending_scopes) {
    for (Library* lib : *scope) if (ContainsAddress(lib, address)) return lib;
  }
  return nullptr;
}
Namespace* ContextNamespace(const LoaderContext& context, Library* caller, std::string* error) {
  if (context.name_space != nullptr) {
    if (!KnownNamespace(context.name_space)) { Fail(error, "unknown loader namespace"); return nullptr; }
    return context.name_space;
  }
  if (caller != nullptr) return caller->name_space;
  return g_anonymous_namespace != nullptr ? g_anonymous_namespace : DefaultNamespaceLocked();
}
Architecture ContextArchitecture(const LoaderContext& context, const Library* caller) {
  return context.architecture == Architecture::kAuto ? LibraryArchitecture(caller) : context.architecture;
}
bool ValidFlags(int flags, std::string* error) {
  if ((flags & ~(kRtldLazy | kRtldNow | kRtldNoLoad | kRtldGlobal | kRtldNoDelete)) == 0) return true;
  Fail(error, "invalid flags to dlopen");
  return false;
}
Library* FindSonameIn(Namespace* ns, const std::string& name, Architecture architecture,
                      const std::vector<Library*>* pending = nullptr) {
  if (name.empty() || HasPath(name)) return nullptr;
  for (Library* lib : ns->libraries) {
    if (lib->soname == name && Available(lib) && MatchesArchitecture(lib, architecture)) return lib;
  }
  if (pending != nullptr) {
    for (Library* lib : *pending) {
      if (lib->name_space == ns && lib->soname == name && MatchesArchitecture(lib, architecture)) return lib;
    }
  }
  return nullptr;
}
Library* FindVisibleSoname(Namespace* ns, const std::string& name, Architecture architecture,
                           const std::vector<Library*>* pending = nullptr) {
  if (Library* lib = FindSonameIn(ns, name, architecture, pending)) return lib;
  for (const auto& link : ns->links) {
    if (LinkedName(link, name)) {
      if (Library* lib = FindSonameIn(link.target, name, architecture, pending)) return lib;
    }
  }
  return nullptr;
}
Library* FindIdentityIn(Namespace* ns, const SourceIdentity& identity, Architecture architecture,
                        const std::vector<Library*>* pending) {
  for (Library* lib : ns->libraries) {
    if (Available(lib) && MatchesArchitecture(lib, architecture) && lib->identity.Matches(identity)) return lib;
  }
  if (pending != nullptr) {
    for (Library* lib : *pending) {
      if (lib->name_space == ns && MatchesArchitecture(lib, architecture) && lib->identity.Matches(identity))
        return lib;
    }
  }
  return nullptr;
}
Library* FindVisibleIdentity(Namespace* ns, const SourceIdentity& identity,
                             Architecture architecture, const std::vector<Library*>* pending) {
  if (Library* lib = FindIdentityIn(ns, identity, architecture, pending)) return lib;
  for (const auto& link : ns->links) {
    if (Library* lib = FindIdentityIn(link.target, identity, architecture, pending)) {
      if (LinkedName(link, lib->soname)) return lib;
    }
  }
  return nullptr;
}
std::string ExpandRunpath(std::string path, const std::string& origin) {
  for (const auto& replacement : {std::pair<const char*, std::string>{"ORIGIN", origin}, {"LIB", "lib64"}}) {
    for (const std::string& token : {std::string("${") + replacement.first + "}",
                                    std::string("$") + replacement.first}) {
      for (size_t at = 0; (at = path.find(token, at)) != std::string::npos;) {
        const size_t next = at + token.size();
        if (token.back() != '}' && next < path.size() &&
            ((path[next] >= 'A' && path[next] <= 'Z') || (path[next] >= 'a' && path[next] <= 'z') ||
             path[next] == '_' || (path[next] >= '0' && path[next] <= '9'))) { at = next; continue; }
        path.replace(at, token.size(), replacement.second);
        at += replacement.second.size();
      }
    }
  }
  return path;
}
void ParseRunpath(Library* lib, const char* value) {
  for (auto path : SplitPaths(value)) {
    path = ExpandRunpath(std::move(path), DirectoryName(lib->realpath));
    struct stat info {};
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) lib->runpaths.push_back(CanonicalPath(path));
  }
}

// Virtual dependencies are other actual registered implementations. Snapshot
// all edges before changing any node, so an unavailable required target cannot
// leave a partially updated provider graph visible after a failed operation.
bool RefreshProviderGraph(Library* root, Architecture architecture, std::string* error) {
  if (!root->provider) return true;
  struct Edges { Library* lib; std::vector<Library*> dependencies; };
  std::vector<Edges> updates;
  std::vector<Library*> queue{root};
  for (size_t i = 0; i < queue.size(); ++i) {
    Library* lib = queue[i];
    if (!Available(lib) || !MatchesArchitecture(lib, architecture)) {
      Fail(error, "unavailable provider: " + lib->soname); return false;
    }
    const auto names = lib->provider->dependency_snapshot
        ? lib->provider->dependency_snapshot() : lib->provider->dependencies;
    Edges update{lib, {}};
    for (const auto& name : names) {
      Library* dependency = FindSonameIn(DefaultNamespaceLocked(), name, architecture);
      if (!dependency || !dependency->provider) {
        Fail(error, lib->soname + ": unavailable required provider " + name); return false;
      }
      update.dependencies.push_back(dependency);
      if (std::find(queue.begin(), queue.end(), dependency) == queue.end()) queue.push_back(dependency);
    }
    updates.push_back(std::move(update));
  }
  for (auto& update : updates) update.lib->dependencies = std::move(update.dependencies);
  return true;
}
Library* FindProviderAlias(Namespace* ns, const std::string& path, Architecture architecture,
                           bool linked) {
  auto find = [&](Namespace* owner) -> Library* {
    for (Library* lib : owner->libraries) {
      if (!lib->provider || !Available(lib) || !MatchesArchitecture(lib, architecture)) continue;
      const auto& aliases = lib->provider->path_aliases;
      if (std::find(aliases.begin(), aliases.end(), path) != aliases.end()) return lib;
    }
    return nullptr;
  };
  if (Library* lib = find(ns)) return lib;
  if (!linked) return nullptr;
  for (const auto& link : ns->links) {
    if (Library* lib = find(link.target)) if (LinkedName(link, lib->soname)) return lib;
  }
  return nullptr;
}
bool ExemptLibrary(Namespace* ns, const std::string& name, const Library* needed_by, int sdk) {
  if (sdk >= 24) return false;
  bool system_parent = false;
  if (needed_by) {
    for (const auto& directory : DefaultNamespaceLocked()->options.default_library_paths) {
      if (DirectoryName(needed_by->realpath) == directory) system_parent = true;
    }
  }
  if (system_parent) {
    for (const auto& link : ns->links) if (LinkedName(link, BaseName(name))) return false;
    return true;
  }
  std::string candidate = DirectoryName(name) == "/system/lib64" ? BaseName(name) : name;
  static const char* const names[] = {"libandroid_runtime.so", "libbinder.so", "libcrypto.so",
      "libcutils.so", "libexpat.so", "libgui.so", "libmedia.so", "libnativehelper.so", "libssl.so",
      "libstagefright.so", "libsqlite.so", "libui.so", "libutils.so"};
  for (const char* allowed : names) if (candidate == allowed) return true;
  return false;
}
