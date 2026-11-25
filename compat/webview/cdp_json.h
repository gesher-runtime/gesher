// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Bounded structured CDP JSON shared by the native bridge and its probes.
#pragma once

#include <cwchar>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gesher::webview::cdp_json {

inline constexpr size_t kDefaultMaxJson = 65536;

// Bounded JSON for the few CDP responses below. Keep targetId and type in the
// same parsed object; substring/regex matching could attach an unrelated target.
struct Json {
  wchar_t kind = 0;
  std::wstring text;
  std::map<std::wstring, Json> fields;
  std::vector<Json> items;
  const Json* Get(const wchar_t* key, wchar_t type) const {
    auto found = fields.find(key);
    return kind == L'{' && found != fields.end() && found->second.kind == type
               ? &found->second : nullptr;
  }
};

class JsonReader {
 public:
  explicit JsonReader(std::wstring_view input, size_t max_bytes = kDefaultMaxJson, unsigned max_nodes = 2048)
      : input_(input), max_bytes_(max_bytes), max_nodes_(max_nodes) {}
  bool Read(Json* value) {
    return input_.size() <= max_bytes_ && Value(value, 0) && (Space(), at_ == input_.size());
  }
 private:
  void Space() { while (at_ < input_.size() && (input_[at_] == L' ' || input_[at_] == L'\t' ||
      input_[at_] == L'\r' || input_[at_] == L'\n')) ++at_; }
  bool Take(wchar_t c) { Space(); if (at_ == input_.size() || input_[at_] != c) return false; ++at_; return true; }
  bool String(std::wstring* out) {
    if (!Take(L'"')) return false;
    while (at_ < input_.size()) {
      wchar_t c = input_[at_++];
      if (c == L'"') return true;
      if (c < 0x20) return false;
      if (c == L'\\') {
        if (at_ == input_.size()) return false;
        c = input_[at_++];
        switch (c) {
          case L'"': case L'\\': case L'/': break;
          case L'b': c = L'\b'; break; case L'f': c = L'\f'; break;
          case L'n': c = L'\n'; break; case L'r': c = L'\r'; break; case L't': c = L'\t'; break;
          case L'u': {
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              if (at_ == input_.size()) return false;
              wchar_t digit = input_[at_++];
              int n = digit >= L'0' && digit <= L'9' ? digit - L'0' :
                      digit >= L'a' && digit <= L'f' ? digit - L'a' + 10 :
                      digit >= L'A' && digit <= L'F' ? digit - L'A' + 10 : -1;
              if (n < 0) return false;
              code = code * 16 + n;
            }
            c = static_cast<wchar_t>(code); break;
          }
          default: return false;
        }
      }
      out->push_back(c);
    }
    return false;
  }
  bool Value(Json* value, unsigned depth) {
    Space();
    if (depth > 16 || ++nodes_ > max_nodes_ || at_ == input_.size()) return false;
    value->kind = input_[at_];
    if (value->kind == L'"') return String(&value->text);
    if (value->kind == L'{' || value->kind == L'[') {
      const bool object = input_[at_++] == L'{';
      const wchar_t end = object ? L'}' : L']';
      if (Take(end)) return true;
      do {
        Json member; std::wstring key;
        if (object && (!String(&key) || !Take(L':'))) return false;
        if (!Value(&member, depth + 1)) return false;
        if (object) {
          if (!value->fields.emplace(std::move(key), std::move(member)).second) return false;
        } else value->items.push_back(std::move(member));
        if (Take(end)) return true;
      } while (Take(L','));
      return false;
    }
    for (auto literal : {L"true", L"false", L"null"}) {
      std::wstring_view word(literal);
      if (input_.substr(at_, word.size()) == word) { at_ += word.size(); return true; }
    }
    value->kind = L'#';
    size_t start = at_;
    if (input_[at_] == L'-') ++at_;
    if (at_ == input_.size()) return false;
    if (input_[at_] == L'0') ++at_;
    else {
      if (input_[at_] < L'1' || input_[at_] > L'9') return false;
      while (at_ < input_.size() && input_[at_] >= L'0' && input_[at_] <= L'9') ++at_;
    }
    for (wchar_t marker : {L'.', L'e'}) {
      if (at_ == input_.size() || (input_[at_] != marker && !(marker == L'e' && input_[at_] == L'E'))) continue;
      ++at_;
      if (marker == L'e' && at_ < input_.size() && (input_[at_] == L'+' || input_[at_] == L'-')) ++at_;
      size_t digits = at_;
      while (at_ < input_.size() && input_[at_] >= L'0' && input_[at_] <= L'9') ++at_;
      if (at_ == digits) return false;
    }
    value->text = input_.substr(start, at_ - start);
    return true;
  }
  std::wstring_view input_;
  size_t at_ = 0;
  unsigned nodes_ = 0;
  size_t max_bytes_;
  unsigned max_nodes_;
};

inline std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (wchar_t c : value) {
    if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
    else if (c < 0x20) { wchar_t escape[7]; swprintf(escape, 7, L"\\u%04x", unsigned(c)); result += escape; }
    else result += c;
  }
  return result + L'"';
}

}  // namespace gesher::webview::cdp_json
