/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Test-only scheduling observation. Include before the unchanged adapter TU:
// signal immediately before entering the real metadata operation lock. No
// lock or namespace policy is substituted. CreateNamespace observation also
// makes the former registry->ELF order a deterministic deadlock control.
#pragma once
#include <gesher/elf_loader.h>

namespace gesher::elf {
void ObserveNamespaceOperationAttempt();
class ObservedScopedLoaderOperation {
  struct Signal { Signal() { ObserveNamespaceOperationAttempt(); } } signal_;
  ScopedLoaderOperation operation_;
 public:
  ObservedScopedLoaderOperation() = default;
  ObservedScopedLoaderOperation(const ObservedScopedLoaderOperation&) = delete;
  ObservedScopedLoaderOperation& operator=(const ObservedScopedLoaderOperation&) = delete;
};
Namespace* ObservedCreateNamespace(const NamespaceOptions&, std::string*);
std::vector<ProviderInfo> ObservedAvailableProviders();
}

#define ScopedLoaderOperation ObservedScopedLoaderOperation
#define CreateNamespace ObservedCreateNamespace
#define AvailableProviders ObservedAvailableProviders
