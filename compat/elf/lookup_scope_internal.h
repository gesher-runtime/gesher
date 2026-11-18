// Namespace-aware symbol candidates and breadth-first handle/local groups.
#pragma once
ResolvedSymbol LookupCandidate(Library* lib, const char* name, uint32_t gh, uint32_t sh,
                               const SymbolVersion* version, Architecture architecture) {
  if (!Available(lib) || !MatchesArchitecture(lib, architecture)) return {};
  if (lib->provider) {
    if (!RefreshProviderGraph(lib, architecture, nullptr)) return {};
    if (void* value = lib->provider->lookup(name)) return {value, nullptr, nullptr, true};
    return {};
  }
  const Elf64_Sym* symbol = FindDefinedSymbol(lib, name, gh, sh, version);
  return symbol ? ResolvedSymbol{LookupSymbolAddress(lib, symbol), lib, symbol, true} : ResolvedSymbol{};
}
ResolvedSymbol LookupGraph(Library* root, Namespace* ns, const char* name,
                           const SymbolVersion* version, Architecture architecture,
                           Library* skip_until = nullptr) {
  if (!root || !Available(root) || !MatchesArchitecture(root, architecture) ||
      (root->provider && !RefreshProviderGraph(root, architecture, nullptr))) return {};
  const uint32_t gh = GnuHash(name), sh = SysvHash(name);
  bool searching = skip_until == nullptr;
  auto visit = [&](Library* lib) -> ResolvedSymbol {
    if (!searching) { if (lib == skip_until) searching = true; return {}; }
    return LookupCandidate(lib, name, gh, sh, version, architecture);
  };
  if (auto found = visit(root); found.found) return found;
  // Invocation-owned queue preserves the allocation-free small graph path.
  constexpr size_t kInlineDependencies = 16;
  Library* inline_pending[kInlineDependencies];
  size_t inline_count = 0;
  std::vector<Library*> pending;
  std::set<Library*> seen;
  auto enqueue = [&](Library* owner) {
    for (Library* dep : owner->dependencies) {
      if (seen.empty()) {
        if (dep == root || std::find(inline_pending, inline_pending + inline_count, dep) !=
                              inline_pending + inline_count) continue;
        if (inline_count < kInlineDependencies) { inline_pending[inline_count++] = dep; continue; }
        seen.insert(root);
        seen.insert(inline_pending, inline_pending + inline_count);
        pending.assign(inline_pending, inline_pending + inline_count);
      }
      if (seen.insert(dep).second) pending.push_back(dep);
    }
  };
  enqueue(root);
  for (size_t i = 0; i < (seen.empty() ? inline_count : pending.size()); ++i) {
    Library* dep = seen.empty() ? inline_pending[i] : pending[i];
    // Bionic walks without looking up until NEXT reaches its caller. Once
    // lookup starts, an inaccessible object's private descendants are skipped.
    if (searching && !Accessible(ns, dep, root->load_scope)) continue;
    if (!Available(dep) || !MatchesArchitecture(dep, architecture) ||
        (dep->provider && !RefreshProviderGraph(dep, architecture, nullptr))) continue;
    if (auto found = visit(dep); found.found) return found;
    enqueue(dep);
  }
  return {};
}
ResolvedSymbol LookupLinear(Namespace* ns, Library* caller, bool next,
                            const char* name, const SymbolVersion* version,
                            Architecture architecture) {
  const uint32_t gh = GnuHash(name), sh = SysvHash(name);
  bool searching = !next;
  auto visit = [&](Library* lib) -> ResolvedSymbol {
    if (!searching) { if (lib == caller) searching = true; return {}; }
    if ((lib->rtld_flags & kRtldGlobal) == 0 && lib->target_sdk_version >= 23) return {};
    return LookupCandidate(lib, name, gh, sh, version, architecture);
  };
  for (Library* lib : ns->libraries) {
    if (auto found = visit(lib); found.found) return found;
  }
  if (caller && caller->load_scope) {
    for (Library* lib : *caller->load_scope) {
      if (lib->name_space == ns) if (auto found = visit(lib); found.found) return found;
    }
  }
  if (caller && caller->local_group_root) {
    Library* root = caller->local_group_root;
    return LookupGraph(root, root->name_space, name, version, architecture, next ? caller : nullptr);
  }
  return {};
}
