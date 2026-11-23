// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Real Windows COM/FTM cross-apartment lifetime gate. No WebView is created.
#include "../../compat/webview/owned_stream.h"

#include <array>
#include <cstdio>
#include <thread>

namespace {
std::atomic<int> failures{0}, checks{0};
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL  owned stream: %s\n", label); }
}
uint8_t Byte(size_t at) { return static_cast<uint8_t>((at * 37 + 11) % 251); }
}

int main() {
  using gesher::webview::OwnedStream;
  HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Check(SUCCEEDED(apartment), "source STA initialized");
  if (FAILED(apartment)) return 1;

  constexpr size_t length = 262147;
  std::vector<uint8_t> bytes(length);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = Byte(i);
  auto body = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
  std::weak_ptr<const std::vector<uint8_t>> lifetime = body;
  IStream* source = nullptr;
  Check(OwnedStream::Create(body, &source) == S_OK && source != nullptr, "source created");
  body.reset();
  if (source != nullptr) {
    IAgileObject* agile = nullptr;
    Check(source->QueryInterface(IID_IAgileObject, reinterpret_cast<void**>(&agile)) == S_OK,
          "agility advertised");
    if (agile != nullptr) agile->Release();
    IMarshal* marshaler = nullptr;
    Check(source->QueryInterface(IID_IMarshal, reinterpret_cast<void**>(&marshaler)) == S_OK,
          "actual free-threaded marshaler exposed");
    if (marshaler != nullptr) {
      IUnknown* identity = nullptr;
      Check(marshaler->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)) == S_OK &&
            identity == static_cast<IUnknown*>(source), "FTM preserves controlling identity");
      if (identity != nullptr) identity->Release();
      marshaler->Release();
    }
    IStream* packet = nullptr;
    HRESULT marshaled = CoMarshalInterThreadInterfaceInStream(IID_IStream, source, &packet);
    Check(marshaled == S_OK && packet != nullptr, "marshal content to another apartment");
    source->Release();
    if (SUCCEEDED(marshaled) && packet != nullptr) {
      Check(!lifetime.expired(), "marshaled reference keeps backing alive");
      std::thread reader([packet] {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Check(SUCCEEDED(initialized), "background MTA initialized");
        if (FAILED(initialized)) { packet->Release(); return; }
        IStream* stream = nullptr;
        HRESULT received = CoGetInterfaceAndReleaseStream(packet, IID_IStream,
            reinterpret_cast<void**>(&stream));
        Check(received == S_OK && stream != nullptr, "unmarshal content on background MTA");
        if (stream != nullptr) {
          STATSTG stat{};
          Check(stream->Stat(&stat, STATFLAG_NONAME) == S_OK && stat.cbSize.QuadPart == length,
                "background reader observes complete body");
          std::array<uint8_t, 997> block;
          size_t at = 0;
          for (;;) {
            ULONG count = 0;
            HRESULT hr = stream->Read(block.data(), static_cast<ULONG>(block.size()), &count);
            Check(SUCCEEDED(hr) && count <= block.size(), "background Read succeeds");
            if (FAILED(hr) || count > block.size()) break;
            for (ULONG i = 0; i < count; ++i) Check(at + i < length && block[i] == Byte(at + i), "background bytes exact");
            at += count;
            if (hr == S_FALSE || count == 0) break;
          }
          Check(at == length, "background reader receives whole payload");
          IStream* clone = nullptr;
          Check(stream->Clone(&clone) == S_OK && clone != nullptr, "clone in receiving apartment");
          stream->Release();
          if (clone != nullptr) {
            LARGE_INTEGER last{};
            last.QuadPart = -1;
            Check(clone->Seek(last, STREAM_SEEK_END, nullptr) == S_OK, "clone seeks after original release");
            uint8_t tail[2] = {0, 0xaa}; ULONG got = 0;
            Check(clone->Read(tail, 2, &got) == S_FALSE && got == 1 && tail[0] == Byte(length - 1) && tail[1] == 0xaa,
                  "clone retains exact body and short-read boundary");
            clone->Release();
          }
        }
        CoUninitialize();
      });
      reader.join();
    }
  }
  Check(lifetime.expired(), "last cross-apartment reference releases backing");
  CoUninitialize();
  std::printf("%s  owned stream Windows COM: %d checks, %d failures\n",
      failures == 0 ? "PASS" : "FAIL", checks.load(), failures.load());
  return failures == 0 ? 0 : 1;
}
