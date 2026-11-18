// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Original valid ELF objects through the production loader's public API.
// Usage: elf_namespaces_test <fixture-root> <case> [--arm64]
#include <gesher/elf_loader.h>
#ifdef _WIN32
#include <gesher/arm64_runtime.h>
#endif
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__x86_64__)
#define NS_SYSV __attribute__((sysv_abi))
#else
#define NS_SYSV
#endif
namespace elf = gesher::elf;
namespace {
std::string root;
bool arm64;
int checks;
std::array<std::atomic<int>, 32> events{};
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL namespaces (%d): %s\n", checks, label); std::exit(1); }
}
struct Watchdog {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::thread thread{[this] {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(90), [this] { return done; })) std::_Exit(124);
  }};
  ~Watchdog() {
    { std::lock_guard<std::mutex> lock(mutex); done = true; }
    changed.notify_all(); thread.join();
  }
};
std::string Path(const std::string& path) { return root + "/" + path; }
void NS_SYSV Event(int id) {
  if (id < 0 || id >= static_cast<int>(events.size())) std::abort();
  ++events[id];
}
void RegisterEvents() {
  elf::ProviderOptions provider;
  provider.soname = "libns_test_host.so";
  provider.lookup = [](const char* name) -> void* {
    return std::strcmp(name, "ns_event") == 0 ? reinterpret_cast<void*>(&Event) : nullptr;
  };
  std::string error;
  Check(elf::RegisterProvider(provider, &error) != nullptr, "register isolated owned event provider");
}
elf::Namespace* Namespace(const char* name, std::vector<std::string> paths = {},
                          int sdk = 36, bool isolated = true) {
  elf::NamespaceOptions options;
  options.name = name; options.target_sdk_version = sdk; options.isolated = isolated;
  for (const auto& path : paths) options.default_library_paths.push_back(Path(path));
  options.permitted_paths.push_back(root);
  std::string error;
  auto* result = elf::CreateNamespace(options, &error);
  Check(result != nullptr, "create explicit namespace");
  Check(elf::LinkNamespaces(result, elf::DefaultNamespace(), {"libns_test_host.so"}, &error),
        "link only declared test event provider");
  return result;
}
elf::LoadOptions Options(elf::Namespace* ns, int flags = 2) {
  elf::LoadOptions result;
  result.context.name_space = ns;
  result.context.architecture = arm64 ? elf::Architecture::kAarch64 : elf::Architecture::kX86_64;
  result.flags = flags;
  return result;
}
elf::Library* Load(elf::Namespace* ns, const std::string& path, int flags = 2) {
  std::string error;
  auto* lib = elf::Load(path.c_str(), &error, Options(ns, flags));
  if (!lib) std::fprintf(stderr, "load %s: %s\n", path.c_str(), error.c_str());
  Check(lib != nullptr && error.empty(), "valid library loads without a stale diagnostic");
  return lib;
}
void* Entry(void* address) {
#ifdef _WIN32
  if (arm64 && address) address = gesher::arm64::HostEntry(address);
#endif
  return address;
}
int Call(void* address) {
  address = Entry(address);
  Check(address != nullptr, "call has a real selected entry point");
  return reinterpret_cast<int(NS_SYSV*)()>(address)();
}
int Call(elf::Library* lib, const char* name) { return Call(elf::Lookup(lib, name)); }
void* Default(elf::Namespace* ns, const char* name, const void* caller = nullptr) {
  auto context = Options(ns).context;
  context.caller_address = caller;
  return elf::LookupScoped(nullptr, name, nullptr, elf::LookupKind::kDefault, context, nullptr);
}
size_t Count() {
  size_t count = 0;
  elf::ForEachLoaded([&](const elf::PhdrInfo&) { ++count; return 0; });
  return count;
}
void Constructors(elf::Library* lib) {
  Check(Call(lib, "ns_constructor_count") == 1, "real initializer executes exactly once");
}
void Identity() {
  auto* a = Namespace("identity-a", {"identity/a"});
  auto* first = Load(a, Path("identity/a/libns_identity.so"));
  Check(Call(first, "ns_value") == 101, "first same-SONAME file has its own value");
  auto* second = Load(a, Path("identity/b/libns_identity.so"));
  auto* third = Load(a, Path("identity/other/libns_other_filename.so"));
  Check(first != second && second != third && first != third,
        "explicit distinct files do not coalesce by basename or SONAME");
  Check(Call(second, "ns_value") == 202 && Call(third, "ns_value") == 303,
        "distinct same-SONAME files keep their actual code and data");
  elf::AddressInfo first_info, second_info;
  void* first_entry = elf::Lookup(first, "ns_value");
  Check(elf::FindAddress(first_entry, &first_info) &&
        elf::FindAddress(elf::Lookup(second, "ns_value"), &second_info) &&
        std::strstr(first_info.soname, "/identity/a/") &&
        std::strstr(second_info.soname, "/identity/b/"),
        "address metadata identifies actual different resolved source paths");
  Check(elf::FindCaller(first_entry) == first && elf::LibraryNamespace(first) == a,
        "actual ELF instruction address identifies its primary namespace");
  Check(Load(a, "libns_identity.so") == first, "bare SONAME selects first visible identity");
  const std::string original = Path("identity/a/libns_identity.so");
  const std::string hard = original + ".hard", symbolic = original + ".symbolic";
  unlink(hard.c_str()); unlink(symbolic.c_str());
  Check(link(original.c_str(), hard.c_str()) == 0, "create a genuine hard-link alias");
  Check(symlink("libns_identity.so", symbolic.c_str()) == 0, "create a genuine relative symbolic alias");
  Check(Load(a, hard) == first && Load(a, symbolic) == first &&
        Load(a, Path("identity/a/../a/libns_identity.so")) == first,
        "opened-file identity coalesces hard, symbolic and lexical aliases");
  unlink(hard.c_str()); unlink(symbolic.c_str());
  Check(events[11] == 1 && events[12] == 1 && events[13] == 1,
        "aliases do not run constructors again");
  Constructors(first); Constructors(second); Constructors(third);
  auto* b = Namespace("identity-b", {"identity/a"});
  auto* separate = Load(b, original);
  Check(separate != first && events[11] == 2,
        "unlinked namespaces have separate instances of the same file");
  auto* c = Namespace("identity-linked");
  std::string error;
  Check(elf::LinkNamespaces(c, a, {"libns_identity.so"}, &error), "link one actual file SONAME");
  Check(Load(c, "libns_identity.so") == first && Load(c, original) == first,
        "allowed linked namespace reuses actual visible file identity");
  Check(elf::LibraryNamespace(first) == a && elf::LibraryNamespace(separate) == b,
        "primary namespace stays attached to each identity");
  auto* modern = Namespace("missing-soname-modern", {"no-soname/a"});
  auto* n1 = Load(modern, Path("no-soname/a/libns_no_soname.so"));
  auto* n2 = Load(modern, Path("no-soname/b/libns_no_soname.so"));
  Check(n1 != n2 && std::strcmp(elf::Soname(n1), "") == 0 && Call(n2, "ns_value") == 505,
        "modern missing SONAME stays absent and does not merge distinct files");
  Check(Load(modern, "libns_no_soname.so") == n1,
        "bare missing-SONAME request opens a file and reuses its identity");
  auto* legacy = Namespace("missing-soname-22", {"no-soname/a"}, 22);
  elf::SetDefaultTargetSdkVersion(22);
  auto* old = Load(legacy, Path("no-soname/a/libns_no_soname.so"));
  Check(std::strcmp(elf::Soname(old), "libns_no_soname.so") == 0,
        "pre-23 target keeps Bionic basename fallback");
  elf::SetDefaultTargetSdkVersion(36);
  auto* current = Load(legacy, Path("no-soname/b/libns_no_soname.so"));
  Check(current != old && std::strcmp(elf::Soname(current), "") == 0,
        "old namespace creation SDK does not override current process SDK for new SONAME behavior");
  Check(Call(Default(legacy, "ns_value")) == 404,
        "old local group retains its original load-time SDK visibility");
  elf::Close(old);
  Check(!Default(legacy, "ns_value"),
        "new local group uses modern DEFAULT exclusion even in an old namespace");
}
void Memory() {
  auto* ns = Namespace("memory", {}, 22);
  std::ifstream stream(Path("search/ld/libns_leaf.so"), std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), {}};
  Check(!bytes.empty(), "read ordinary compiler-built ELF bytes");
  auto options = Options(ns);
  std::string error;
  auto memory = [&] {
    auto* lib = elf::LoadFromMemory("same-display-name", bytes.data(), bytes.size(), &error, options);
    Check(lib && error.empty(), "valid memory ELF loads"); return lib;
  };
  auto* first = memory(); auto* second = memory();
  Check(!Default(ns, "ns_leaf_value"), "memory load also snapshots current process SDK rather than namespace creation SDK");
  Check(first != second, "diagnostic name and SONAME alone do not identify a memory source");
  int token;
  options.memory_identity = &token;
  auto* identified = memory();
  Check(memory() == identified && identified != first && identified != second,
        "only explicit memory identity requests reuse the same source");
  Constructors(first); Constructors(second); Constructors(identified);
  options.flags |= 4;
  Check(memory() == identified, "NOLOAD recognizes an explicit memory identity");
  options.memory_identity = nullptr;
  Check(elf::LoadFromMemory("same-display-name", bytes.data(), bytes.size(), &error, options) == nullptr,
        "NOLOAD cannot invent identity from a display name");
  const size_t before = Count();
  options.flags = 0x200000;
  Check(!elf::Load(nullptr, &error, options) && !error.empty(), "invalid flags also reject main-handle open");
  Check(Count() == before, "rejected memory and flag requests publish nothing");
  for (int flags : {0, 1, 2, 3}) {
    options.flags = flags;
    Check(elf::Load(nullptr, &error, options) != nullptr && error.empty(),
          "Bionic accepts all valid NOW/LAZY mask combinations");
  }
}
void Search() {
  auto* run = Namespace("runpath", {"search/default"});
  auto* lib = Load(run, Path("search/root/libns_search.so"));
  Check(Call(lib, "ns_read") == 62, "requester's ORIGIN RUNPATH precedes namespace defaults");
  auto* token = Namespace("lib-token", {"search/default"});
  Check(Call(Load(token, Path("search/lib-root/libns_lib.so")), "ns_read") == 64,
        "ORIGIN and LIB expand for actual 64-bit ELF search");
  elf::NamespaceOptions config;
  config.name = "ld-first"; config.permitted_paths = {root};
  config.ld_library_paths = {Path("search/ld")}; config.default_library_paths = {Path("search/default")};
  std::string error;
  auto* ld = elf::CreateNamespace(config, &error);
  Check(ld != nullptr, "create distinct LD override namespace");
  Check(Call(Load(ld, Path("search/root/libns_search.so")), "ns_read") == 61,
        "LD override precedes requester RUNPATH");
  auto* chain = Namespace("nontransitive-runpath", {"search/default"});
  Check(Call(Load(chain, Path("search/chain/libns_chain.so")), "ns_read") == 63,
        "root RUNPATH is not inherited by a grandchild request");
  auto* own = Namespace("transitive-own-runpath", {"search/default"});
  Check(Call(Load(own, Path("search/chain/libns_chain_origin.so")), "ns_read") == 65,
        "a child uses its own declared RUNPATH");
  auto defaults = elf::DefaultLibraryPath();
  elf::SetDefaultLibraryPath({Path("search/ld")});
  Check(elf::DefaultLibraryPath() == defaults, "LD update does not overwrite default-path getter");
  Check(Call(Load(elf::DefaultNamespace(), "libns_leaf.so"), "ns_leaf_value") == 61,
        "process LD update controls new ordinary search");
  elf::SetDefaultLibraryPath({});
  Check(elf::DefaultLibraryPath() == defaults, "clearing LD override preserves configured defaults");
}
void Scope() {
  auto* ns = Namespace("scope", {"scope"});
  auto* global = Load(ns, Path("scope/libns_global.so"), 2 | 0x100);
  Check(Call(Default(ns, "ns_collision")) == 10, "RTLD_GLOBAL participates in namespace DEFAULT");
  auto* imported = Load(ns, Path("scope/libns_import.so"));
  Check(Call(imported, "ns_read") == 20,
        "ordinary RTLD_GLOBAL is not a relocation DF_1_GLOBAL interposer");
  auto* dfglobal = Load(ns, Path("scope/libns_dfglobal.so"));
  auto* plain = Load(ns, Path("scope/libns_plain.so"));
  Check(Call(plain, "ns_bind_self") == 70,
        "actual DF_1_GLOBAL precedes a default-visibility local definition");
  Check(Call(plain, "ns_collision") == 30,
        "explicit handle lookup still starts at the selected object");
  Check(Call(Load(ns, Path("scope/libns_symbolic.so")), "ns_bind_self") == 40,
        "DT_SYMBOLIC keeps its own binding ahead of the global group");
  Check(Call(Load(ns, Path("scope/libns_protected.so")), "ns_bind_self") == 50,
        "protected references bind to their actual defining object");
  auto* hidden = Load(ns, Path("scope/libns_hidden.so"));
  Check(Call(hidden, "ns_bind_self") == 60 && elf::Lookup(hidden, "ns_collision") == nullptr,
        "hidden local symbol is callable internally and absent from exports");
  elf::Close(global); elf::Close(dfglobal);
  Check(Call(Default(ns, "ns_collision")) == 10,
        "Bionic RTLD_GLOBAL/NODELETE objects stay resident after close");
  auto* bfs = Namespace("bfs", {"scope"});
  auto* graph = Load(bfs, Path("scope/libns_bfs.so"), 2 | 0x100);
  Check(Call(graph, "ns_read") == 81 && Call(graph, "ns_collision") == 81,
        "relocation and handle lookup prefer a direct sibling to a grandchild");
  Check(Call(Default(bfs, "ns_collision")) == 81,
        "publication retains breadth-first order for DEFAULT lookup");
  auto* old = Namespace("sdk22", {"scope"}, 22);
  auto* modern = Namespace("sdk36", {"scope"}, 36);
  elf::SetDefaultTargetSdkVersion(22);
  Load(old, Path("scope/libns_local.so"));
  elf::SetDefaultTargetSdkVersion(36);
  auto* local = Load(modern, Path("scope/libns_local.so"));
  Check(Call(Default(old, "ns_collision")) == 20 && !Default(modern, "ns_collision"),
        "RTLD_LOCAL visibility follows each loaded object's target SDK");
  Check(Call(Default(modern, "ns_collision", elf::Lookup(local, "ns_bind_self"))) == 20,
        "identified caller can search its own local group");
  elf::SetDefaultTargetSdkVersion(22);
  auto* old_caller = Load(elf::DefaultNamespace(), Path("scope/libns_local.so"));
  elf::SetDefaultTargetSdkVersion(36);
  auto current = Options(elf::DefaultNamespace());
  current.context.caller_address = elf::Lookup(old_caller, "ns_bind_self");
  std::string error;
  auto* current_lib = elf::Load(Path("scope/libns_plain.so").c_str(), &error, current);
  Check(current_lib != nullptr && Call(Default(elf::DefaultNamespace(), "ns_collision")) == 20,
        "each loaded object retains the target SDK in effect when it loaded");
  elf::Close(old_caller);
  Check(!Default(elf::DefaultNamespace(), "ns_collision"),
        "new load uses current target SDK instead of inheriting an old caller's SDK");
}
void Groups() {
  auto* parent = Namespace("parent", {"scope"});
  auto* global = Load(parent, Path("scope/libns_global.so"), 2 | 0x100);
  auto* local = Load(parent, Path("scope/libns_local.so"));
  elf::NamespaceOptions config;
  config.name = "ordinary-child"; config.parent = parent;
  std::string error;
  auto* child = elf::CreateNamespace(config, &error);
  Check(child && Call(Default(child, "ns_collision")) == 10,
        "ordinary child inherits a nondefault parent's RTLD_GLOBAL group");
  Check(Load(child, "libns_global.so") == global, "inherited global retains its actual handle");
  Check(!elf::Load("libns_local.so", &error, Options(child)), "ordinary child does not inherit parent LOCAL");
  config.name = "shared-child"; config.shared = true;
  auto* shared = elf::CreateNamespace(config, &error);
  Check(shared && Load(shared, "libns_local.so") == local,
        "shared namespace snapshots parent's loaded local identities and paths");
  auto* future = Load(parent, Path("scope/libns_plain.so"));
  Check(Load(shared, "libns_plain.so") != future,
        "parent's later local load is not retroactively in the shared snapshot");
  auto* process_global = Load(elf::DefaultNamespace(), Path("scope/libns_global.so"), 2 | 0x100);
  auto* process_df = Load(elf::DefaultNamespace(), Path("scope/libns_dfglobal.so"));
  config = {}; config.name = "default-child";
  auto* from_default = elf::CreateNamespace(config, &error);
  Check(from_default && Call(Default(from_default, "ns_collision")) == 70,
        "default parent shares DF_1_GLOBAL, not every RTLD_GLOBAL load");
  Check(Load(from_default, "libns_dfglobal.so") == process_df &&
        !elf::Load("libns_global.so", &error, Options(from_default)),
        "default-parent sharing keeps the two global contracts distinct");
  auto* main = elf::Load(nullptr, &error, Options(from_default));
  Check(main && elf::Lookup(main, "ns_collision") == elf::Lookup(process_global, "ns_collision"),
        "main handle searches process default namespace's global members");
  Check(elf::SetAnonymousNamespace(child, &error), "assign actual anonymous namespace once");
  Check(Call(elf::LookupGlobal("ns_collision")) == 10,
        "unidentified caller uses configured anonymous namespace");
  Check(!elf::SetAnonymousNamespace(shared, &error), "anonymous namespace cannot be silently replaced");
}
void Links() {
  elf::NamespaceOptions config;
  config.name = "isolated-search"; config.default_library_paths = {Path("search/root")};
  std::string error;
  auto* ns = elf::CreateNamespace(config, &error);
  Check(ns != nullptr, "create isolated namespace with exact search directory");
  const size_t before = Count();
  Check(!elf::Load("libns_search.so", &error, Options(ns)) && Count() == before,
        "inaccessible RUNPATH dependency fails atomically");
  Check(!elf::Load(Path("identity/a/libns_identity.so").c_str(), &error, Options(ns)),
        "explicit path cannot escape namespace permissions");
  config.name = "file-public"; config.default_library_paths = {Path("search/default")};
  auto* system = elf::CreateNamespace(config, &error);
  Check(system != nullptr, "public file namespace permits its own search directory");
  Check(elf::LinkNamespaces(ns, system, {"libns_leaf.so"}, &error),
        "link names an actual file provider before it is loaded");
  auto* success = Load(ns, "libns_search.so");
  Check(Call(success, "ns_read") == 63 && Count() == before + 2,
        "allowed file provider loads lazily and permits a clean retry");
  auto* wrong = Namespace("not-linked");
  Check(!elf::Load("libns_leaf.so", &error, Options(wrong)),
        "another namespace cannot reuse an unrelated loaded SONAME");
  auto* middle = Namespace("one-hop-middle");
  auto* outer = Namespace("one-hop-outer");
  Check(elf::LinkNamespaces(middle, system, {"libns_leaf.so"}, &error) &&
        elf::LinkNamespaces(outer, middle, {"libns_leaf.so"}, &error), "create two explicit link edges");
  Check(!elf::Load("libns_leaf.so", &error, Options(outer)),
        "namespace links do not implicitly become transitive");
  auto options = Options(ns, 2 | 4);
  Check(elf::Load("libns_search.so", &error, options) == success,
        "NOLOAD finds a visible committed handle");
  Check(!elf::Load(Path("search/lib-root/libns_lib.so").c_str(), &error, options),
        "NOLOAD never creates a different source");
  elf::SetDefaultTargetSdkVersion(22);
  Check(elf::DefaultTargetSdkVersion() == 22, "target SDK setter and getter share authoritative state");
  elf::SetDefaultTargetSdkVersion(0);
  Check(elf::DefaultTargetSdkVersion() == 36, "zero target SDK resets to the actual platform API level");
}
int NS_SYSV LeftValue() { return 21; }
int private_value = 91, unowned_value = 92;
bool provider_available = true;
void Providers() {
  const size_t before = Count();
  std::string error;
  elf::ProviderOptions left;
  left.soname = "libns_left.so"; left.dependencies = {"libns_private.so"};
  left.path_aliases = {Path("registered-virtual/libns_left.so")};
  left.lookup = [](const char* name) -> void* {
    return std::strcmp(name, "ns_left") == 0 ? reinterpret_cast<void*>(&LeftValue) : nullptr;
  };
  auto* provider = elf::RegisterProvider(left, &error);
  Check(provider && elf::IsProvider(provider), "register exact owned virtual library identity");
  Check(!elf::Load("libns_left.so", &error, Options(elf::DefaultNamespace())) && !error.empty(),
        "provider with an unavailable required dependency cannot load");
  elf::ProviderOptions dependency;
  dependency.soname = "libns_private.so";
  dependency.available = [] { return provider_available; };
  dependency.lookup = [](const char* name) -> void* {
    return std::strcmp(name, "ns_private_value") == 0 ? &private_value : nullptr;
  };
  Check(elf::RegisterProvider(dependency, &error) != nullptr, "register actual isolated private exports");
  Check(Load(elf::DefaultNamespace(), "libns_left.so") == provider &&
        elf::Lookup(provider, "ns_private_value") == &private_value,
        "handle lookup walks the provider's declared dependency graph");
  Check(elf::LookupVersion(provider, "ns_private_value", "unadvertised") == &private_value,
        "unversioned provider follows Bionic allowance without named version metadata");
  Check(Load(elf::DefaultNamespace(), Path("registered-virtual/libns_left.so")) == provider &&
        !elf::Load(Path("not-an-alias/libns_left.so").c_str(), &error, Options(elf::DefaultNamespace())),
        "only registered exact virtual paths are aliases; matching basename is insufficient");
  Check(Count() == before, "virtual providers never invent program headers or ELF load counts");
  elf::AddressInfo info;
  Check(!elf::FindAddress(&private_value, &info), "virtual data does not fabricate mapped ELF ownership");
  elf::SetBionicResolver([](const char* name) -> void* {
    return std::strcmp(name, "ns_unowned") == 0 ? &unowned_value : nullptr;
  });
  Check(!elf::Lookup(provider, "ns_unowned"), "owned provider lookup cannot use aggregate host exports");
  auto* app = Namespace("provider-app");
  auto* files = Namespace("provider-files", {"scope"});
  Check(elf::LinkNamespaces(app, elf::DefaultNamespace(), {"libns_left.so"}, &error) &&
        elf::LinkNamespaces(app, files, {"libns_right.so"}, &error), "link only two direct public identities");
  auto* graph = Load(app, Path("scope/libns_bfs.so"));
  Check(Call(graph, "ns_read") == 81, "ELF and owned virtual dependencies share ordered graph lookup");
  Check(!elf::Lookup(graph, "ns_private_value"), "foreign provider's private grandchild is not app-visible");
  provider_available = false;
  Check(!elf::Load("libns_left.so", &error, Options(elf::DefaultNamespace())),
        "actual availability change invalidates an incomplete provider graph");
  Check(!elf::Lookup(provider, "ns_left") && !elf::Lookup(provider, "ns_private_value"),
        "retained handle does not expose an unavailable provider graph");
  provider_available = true;
  Check(Load(elf::DefaultNamespace(), "libns_left.so") == provider, "restored implementation permits retry");
  elf::ProviderOptions x86; x86.soname = "libns_x86_only.so"; x86.architecture_mask = 1;
  x86.lookup = dependency.lookup;
  auto* x86_provider = elf::RegisterProvider(x86, &error);
  Check(x86_provider != nullptr, "register actual architecture-specific provider");
  auto options = Options(elf::DefaultNamespace()); options.context.architecture = elf::Architecture::kAarch64;
  Check(!elf::Load("libns_x86_only.so", &error, options), "provider architecture is checked on ordinary load");
  Check(!elf::LookupScoped(x86_provider, "ns_private_value", nullptr, elf::LookupKind::kHandle,
                           options.context, &error), "explicit provider handle honors caller architecture");
  elf::ProviderOptions exempt; exempt.soname = "libcrypto.so"; exempt.lookup = dependency.lookup;
  Check(elf::RegisterProvider(exempt, &error) != nullptr, "register isolated actual exports for SDK exemption test");
  for (int sdk : {23, 24}) {
    elf::SetDefaultTargetSdkVersion(sdk);
    elf::NamespaceOptions config;
    config.name = "exempt-" + std::to_string(sdk); config.target_sdk_version = sdk;
    config.exempt_list_enabled = true;
    auto* ns = elf::CreateNamespace(config, &error);
    Check(ns != nullptr && (elf::Load("libcrypto.so", &error, Options(ns)) != nullptr) == (sdk < 24),
          "compatibility exemption uses an actual available provider only below target24");
  }
  elf::SetDefaultTargetSdkVersion(0);
}
void Concurrent() {
  auto* a = Namespace("concurrent-a", {"identity/a"});
  auto* b = Namespace("concurrent-b", {"identity/a"});
  std::array<elf::Library*, 16> handles{};
  std::array<std::string, 16> errors;
  std::array<std::thread, 16> threads;
  std::mutex mutex; std::condition_variable changed;
  size_t ready = 0; bool start = false;
  for (size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
    { std::unique_lock<std::mutex> lock(mutex); ++ready; changed.notify_all();
      changed.wait(lock, [&] { return start; }); }
    handles[i] = elf::Load(Path("identity/a/libns_identity.so").c_str(), &errors[i], Options(i % 2 ? a : b));
  });
  { std::unique_lock<std::mutex> lock(mutex); changed.wait(lock, [&] { return ready == threads.size(); });
    start = true; }
  changed.notify_all();
  for (auto& thread : threads) thread.join();
  Check(handles[0] && handles[1] && handles[0] != handles[1],
        "concurrent namespaces keep distinct same-file identities");
  for (size_t i = 0; i < handles.size(); ++i) {
    Check(handles[i] == handles[i % 2] && errors[i].empty(),
          "concurrent requests in one namespace share one completed object");
    Constructors(handles[i]);
  }
  Check(events[11] == 2 && Count() == 2,
        "concurrent file reuse runs one initializer per actual namespace instance");
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) return 2;
  root = argv[1]; arm64 = argc == 4 && std::strcmp(argv[3], "--arm64") == 0;
  if (argc == 4 && !arm64) return 2;
  Watchdog watchdog;
  RegisterEvents();
  const std::string test = argv[2];
  if (test == "identity") Identity();
  else if (test == "memory") Memory();
  else if (test == "search") Search();
  else if (test == "scope") Scope();
  else if (test == "groups") Groups();
  else if (test == "links") Links();
  else if (test == "providers") Providers();
  else if (test == "concurrent") Concurrent();
  else return 2;
  std::printf("ELF namespaces %s: %d checks passed (%s)\n", test.c_str(), checks,
              arm64 ? "translated ARM64" : "x86-64");
  return 0;
}
