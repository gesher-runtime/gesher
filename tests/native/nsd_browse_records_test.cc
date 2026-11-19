// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#include "nsd_browse_records.h"
#include <cstdio>

int main() {
  using gesher::nsd::classify_ptr;
  using gesher::nsd::PtrObservation;
  constexpr auto query = L"_printer._sub._owned._tcp.local";
  constexpr auto base = L"_owned._tcp.local";
  struct Case {
    const wchar_t* owner;
    bool withdrawn;
    PtrObservation primary, companion;
  };
  const Case cases[] = {
    {query, false, PtrObservation::Membership, PtrObservation::Membership},
    {query, true, PtrObservation::Membership, PtrObservation::Membership},
    {L"_PRINTER._SUB._OWNED._TCP.LOCAL.", false, PtrObservation::Membership, PtrObservation::Membership},
    {base, false, PtrObservation::Ignore, PtrObservation::Ignore},
    {base, true, PtrObservation::Ignore, PtrObservation::Goodbye},
    {L"_OWNED._TCP.LOCAL.", false, PtrObservation::Ignore, PtrObservation::Ignore},
    {L"_OWNED._TCP.LOCAL.", true, PtrObservation::Ignore, PtrObservation::Goodbye},
    {L"_other._sub._owned._tcp.local", false, PtrObservation::Ignore, PtrObservation::Ignore},
    {L"_other._sub._owned._tcp.local", true, PtrObservation::Ignore, PtrObservation::Ignore},
    {L"_unrelated._tcp.local", false, PtrObservation::Ignore, PtrObservation::Ignore},
    {L"_unrelated._tcp.local", true, PtrObservation::Ignore, PtrObservation::Ignore},
  };
  unsigned checks = 0;
  for (const auto& test : cases) {
    for (bool companion : {false, true}) {
      if (classify_ptr(test.owner, query, base, test.withdrawn, companion) !=
          (companion ? test.companion : test.primary)) {
        std::fprintf(stderr, "NSD PTR classification failed at case %u\n", checks);
        return 1;
      }
      ++checks;
    }
  }
  if (classify_ptr(base, base, base, false, false) != PtrObservation::Membership ||
      classify_ptr(base, base, base, true, false) != PtrObservation::Goodbye) return 1;
  checks += 2;
  std::printf("nsd_browse_records_test: %u checks passed (production record classifier)\n", checks);
}
