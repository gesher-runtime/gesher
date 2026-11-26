// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Production NlmInfo defaults and cost mapping; no Windows settings are changed.
#include "../../compat/net/nlm_info.h"

#include <cstdio>
#include <map>

namespace {
int checks = 0;
int failures = 0;

void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) ++failures;
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", label);
}

// Machine-readable samples consumed by the ordinary DownloadQueue integration
// fixture. Their booleans come from the same production type as Snapshot().
void Sample(const char* label, const gesher::net::NlmInfo& info) {
  std::printf("sample\t%s\t%d\t%d\t%d\n", label, info.metered ? 1 : 0,
              info.roaming ? 1 : 0, info.congested ? 1 : 0);
}
}  // namespace

int main() {
  using gesher::net::NlmInfo;
  NlmInfo unmatched_adapter;
  Check(unmatched_adapter.metered, "missing NLM or unmatched adapter does not grant unmetered");
  Sample("unmatched", unmatched_adapter);
  std::map<int, NlmInfo> adapters;
  NlmInfo& info = adapters[1];
  Check(info.metered, "a new NLM adapter starts with unknown cost");

  info.SetCost(false, 0);
  Check(info.metered, "missing cost interface is conservative");
  Sample("missing-interface", info);
  info.SetCost(false, NlmInfo::kUnrestricted);
  Check(info.metered, "failed GetCost ignores an unrestricted output value");
  Sample("failed-query", info);
  info.SetCost(true, 0);
  Check(info.metered, "successful UNKNOWN cost is conservative");
  Sample("unknown", info);

  info.SetCost(true, NlmInfo::kUnrestricted);
  Check(!info.metered, "explicit unrestricted cost permits unmetered usage");
  Sample("unrestricted", info);
  info.SetCost(true, NlmInfo::kFixed);
  Check(info.metered, "fixed capacity cost is metered");
  Sample("fixed", info);
  info.SetCost(true, NlmInfo::kVariable);
  Check(info.metered, "per-byte cost is metered");
  Sample("variable", info);
  info.SetCost(true, NlmInfo::kUnrestricted | NlmInfo::kFixed);
  Check(info.metered, "contradictory unrestricted and fixed bits cannot grant unmetered");
  info.SetCost(true, NlmInfo::kUnrestricted | NlmInfo::kVariable);
  Check(info.metered, "contradictory unrestricted and variable bits cannot grant unmetered");

  // Reusing out[adapter] must not retain an earlier unrestricted reading after
  // a later connection for that same adapter fails to supply its cost.
  info.SetCost(true, NlmInfo::kUnrestricted);
  Check(!info.metered, "same adapter starts explicitly unrestricted");
  adapters[1].SetCost(false, NlmInfo::kUnrestricted);
  Check(info.metered, "same adapter query failure revokes unmetered");
  Sample("unrestricted-to-failure", info);
  info.SetCost(true, NlmInfo::kUnrestricted);
  info.SetCost(true, 0);
  Check(info.metered, "same adapter UNKNOWN reading revokes unmetered");
  Sample("unrestricted-to-unknown", info);
  info.SetCost(true, NlmInfo::kUnrestricted);
  Check(!info.metered, "a later confirmed unrestricted reading restores unmetered");
  Sample("restored-unrestricted", info);

  NlmInfo next_snapshot;
  Check(next_snapshot.metered, "a later snapshot without matching cost starts conservative");
  info.internet = true;
  info.captive_portal = true;
  info.SetCost(true, NlmInfo::kUnrestricted | NlmInfo::kRoaming | NlmInfo::kCongested);
  Check(!info.metered && info.roaming && info.congested,
        "roaming and congestion retain their independent successful cost flags");
  info.SetCost(false, 0);
  Check(info.metered && info.roaming && info.congested,
        "query failure revokes unmetered without fabricating other cost flags");
  Check(info.internet && info.captive_portal, "cost classification does not change reachability");
  std::printf("cost-policy %s: %d checks, %d failures\n",
              failures == 0 ? "PASSED" : "FAILED", checks, failures);
  return failures == 0 ? 0 : 1;
}
