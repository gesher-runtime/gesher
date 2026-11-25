// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Run on the build host; actual browser/profile behavior has a separate fixture.
#include "../../compat/webview/cdp_json.h"
#include "../../compat/webview/storage_key_selection.h"

#include <cstdio>

using namespace gesher::webview::storage_key_selection;
using gesher::webview::cdp_json::Json;
using gesher::webview::cdp_json::JsonReader;

namespace {
unsigned checks = 0, failed = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failed; std::fprintf(stderr, "FAIL %s\n", label); }
}
}

int main() {
  const std::wstring a = L"https://a.example/";
  const std::wstring b = L"https://b.example/";
  struct Case { const wchar_t* key; bool removed; };
  const Case cases[] = {
    {L"https://a.example/", true},
    {L"https://a.example:8443/", false},
    {L"https://sub.a.example/", false},
    {L"https://b.example/", false},
    {L"http://a.example/", false},
    {L"https://c.example/^0https://a.example", true},
    {L"https://a.example/^0https://b.example", false},
    {L"https://c.example/^0https://b.example", false},
    {L"https://a.example.evil/^0https://b.example", false},
    {L"https://c.example/^0http://a.example", false},
    {L"chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/^0https://a.example", true},
    {L"https://a.example/^31", true},
    {L"https://sub.a.example/^31", true},
    {L"https://a.example:8443/^31", true},
    {L"https://b.example/^31", false},
    {L"http://a.example/^31", false},
    {L"https://a.example/^11^22", true},
    {L"https://sub.a.example/^118446744073709551615^20", true},
    {L"https://b.example/^13^24", false},
    // An opaque site's precursor is not the top-level site's identity.
    {L"https://c.example/^41^52^6https://a.example", false},
    {L"https://c.example/^41^52^6", false},
  };
  std::vector<Key> keys;
  std::set<std::wstring> expected;
  for (const auto& item : cases) {
    Key key;
    Check(Parse(item.key, &key), "known browser key form parses");
    Check(key.serialized == item.key, "complete original key is retained");
    keys.push_back(std::move(key));
    if (item.removed) expected.insert(item.key);
  }
  // These sites are runtime results supplied to the selector, not a PSL
  // implementation. Private-suffix and IP canonicalization belong to the
  // independently run browser fixture.
  std::map<std::wstring, std::wstring> sites = {
      {a, L"https://a.example"}, {b, L"https://b.example"},
      {L"https://sub.a.example/", L"https://a.example"},
      {L"https://a.example:8443/", L"https://a.example"}};
  std::set<std::wstring> selected;
  Check(Select(keys, a, sites, &selected), "complete selection succeeds");
  Check(selected == expected, "trusted-origin matrix deletes exactly the expected full keys");
  auto missing = sites;
  missing.erase(L"https://sub.a.example/");
  const auto previous = selected;
  Check(!Select(keys, a, missing, &selected), "missing implicit-site resolution fails");
  Check(selected == previous, "incomplete selection never publishes a partial deletion set");
  missing.erase(a);
  Check(!Select(keys, a, missing, &selected), "missing requested site fails");

  const wchar_t* invalid[] = {
      L"", L"https://a.example", L"https://a.example/^0", L"https://a.example/^30",
      L"https://a.example/^31^31", L"https://a.example/^0https://b.example^31",
      L"https://a.example/^0https://b.example/", L"https://a.example/^9unknown",
      L"https://a.example/^11", L"https://a.example/^10^20", L"https://a.example/^101^22",
      L"https://a.example/^118446744073709551616^22", L"https://a.example/^11^2-2",
      L"https://a.example/^11^22^31", L"https://a.example/^41^52",
      L"https://a.example/^41^52^6https://a.example^31", L"https://a.example/\n"};
  for (const wchar_t* item : invalid) {
    Key key;
    Check(!Parse(item, &key), "unknown/malformed key fails before deletion");
  }
  Key key;
  Check(!Parse(std::wstring(L"https://a.example/\0", 19), &key), "embedded NUL is rejected");
  Check(!Parse(L"https://\u00e9.example/", &key), "nonserialized Unicode origin is rejected");

  Json json;
  Check(JsonReader(L"{\"keys\":[\"https://a.example/^0https://b.example\"],\"total\":1}").Read(&json),
        "structured page result parses");
  Json duplicate;
  Check(!JsonReader(L"{\"keys\":[],\"keys\":[]}").Read(&duplicate), "duplicate fields are rejected");
  Json limited;
  Check(!JsonReader(L"{\"keys\":[]}", 5, 100).Read(&limited), "explicit byte limit is enforced");
  Json nodes;
  Check(!JsonReader(L"[1,2,3]", 100, 3).Read(&nodes), "explicit node limit is enforced");
  Json complete;
  Check(JsonReader(L"[1,2,3]", 100, 4).Read(&complete), "exact node limit admits a complete value");
  std::printf("storage-key selection: %u checks, %u failures\n", checks, failed);
  return failed ? 1 : 0;
}
