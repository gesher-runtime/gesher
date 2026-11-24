// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Manual Windows probe for compat/devices/hello_key_win.cc (ADR 0095). It drives the
// production functions against the real Windows Hello and answers the questions the
// design depends on:
//
//   hello_key_probe supported   IsSupportedAsync only; shows no UI
//   hello_key_probe run         creates a key (prompt), signs the same message twice
//                               (a prompt each), says whether the signatures are
//                               identical and which padding verifies them, wraps and
//                               unwraps a test secret with a key derived from the
//                               signature, deletes the key and opens it again
//   hello_key_probe cancel      creates a key, starts a signature and cancels it from
//                               the "app" after 3 s, deletes the key
//
// The wrapping here (SHA-256 of the signature as an AES-256-GCM key) only shows the
// property; production derives and uses its keys in KeystoreAuthorization.java.
// Not run in CI: every mode but "supported" needs a person at the machine.

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <thread>
#include <vector>

#include "hello_key_win.h"

using namespace gesher::devices;

static const char* StatusName(int s) {
  static const char* names[] = {"Success",      "UnknownError",        "NotFound",
                                "UserCanceled", "UserPrefersPassword", "CredentialAlreadyExists",
                                "SecurityDeviceLocked", "AlgorithmNotSupported"};
  if (s == hello_key::kNotForeground) return "NOT_FOREGROUND";
  return s >= 0 && s <= 7 ? names[s] : "FAILED";
}

static HWND MakeWindow() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherHelloKeyProbe";
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"Gesher Windows Hello key probe",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 480,
                           240, nullptr, nullptr, wc.hInstance, nullptr);
  SetForegroundWindow(w);
  return w;
}

static void Pump() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

// Waits for the operation while the window stays alive; cancels after cancelMs (0: never).
static int Run(hello_key::Operation* o, std::vector<uint8_t>* bytes, DWORD cancelMs) {
  int status = -100;
  DWORD started = GetTickCount();
  std::thread waiter([&] { status = hello_key::Await(o, bytes); });
  bool canceled = false;
  while (status == -100) {
    Pump();
    if (cancelMs != 0 && !canceled && GetTickCount() - started > cancelMs) {
      printf("app cancels after %lu ms\n", (unsigned long)cancelMs);
      hello_key::Cancel(o);
      hello_key::Cancel(o);  // idempotent
      canceled = true;
    }
    Sleep(20);
  }
  waiter.join();
  hello_key::Release(o);
  return status;
}

static bool Sha256(const std::vector<uint8_t>& data, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                           static_cast<ULONG>(data.size()), 0) == 0 &&
            BCryptFinishHash(hash, out, 32, 0) == 0;
  if (hash != nullptr) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

// Whether `signature` over `message` verifies under the SubjectPublicKeyInfo with
// PKCS #1 v1.5 (pss false) or PSS with a 32-byte salt (pss true), SHA-256.
static bool Verifies(const std::vector<uint8_t>& spki, const std::vector<uint8_t>& message,
                     const std::vector<uint8_t>& signature, bool pss) {
  CERT_PUBLIC_KEY_INFO* info = nullptr;
  DWORD size = 0;
  if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, spki.data(),
                           static_cast<DWORD>(spki.size()), CRYPT_DECODE_ALLOC_FLAG, nullptr,
                           &info, &size)) {
    return false;
  }
  BCRYPT_KEY_HANDLE key = nullptr;
  bool ok = false;
  uint8_t digest[32];
  if (CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, nullptr, &key) &&
      Sha256(message, digest)) {
    BCRYPT_PKCS1_PADDING_INFO pkcs1 = {BCRYPT_SHA256_ALGORITHM};
    BCRYPT_PSS_PADDING_INFO pssInfo = {BCRYPT_SHA256_ALGORITHM, 32};
    ok = BCryptVerifySignature(key, pss ? static_cast<void*>(&pssInfo) : &pkcs1, digest, 32,
                               const_cast<PUCHAR>(signature.data()),
                               static_cast<ULONG>(signature.size()),
                               pss ? BCRYPT_PAD_PSS : BCRYPT_PAD_PKCS1) == 0;
  }
  if (key != nullptr) BCryptDestroyKey(key);
  LocalFree(info);
  return ok;
}

// AES-256-GCM with a zero nonce (one message per key here). Encrypts or decrypts
// `in` to `out` with the 16-byte tag in `tag`; false when the tag does not verify.
static bool Gcm(bool encrypt, const uint8_t key[32], const std::vector<uint8_t>& in,
                std::vector<uint8_t>* out, uint8_t tag[16]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_KEY_HANDLE handle = nullptr;
  bool ok = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
            BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, const_cast<PUCHAR>(key), 32,
                                       0) == 0;
  if (ok) {
    uint8_t nonce[12] = {};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO mode;
    BCRYPT_INIT_AUTH_MODE_INFO(mode);
    mode.pbNonce = nonce;
    mode.cbNonce = sizeof(nonce);
    mode.pbTag = tag;
    mode.cbTag = 16;
    out->assign(in.size(), 0);
    ULONG done = 0;
    PUCHAR input = const_cast<PUCHAR>(in.data());
    ULONG length = static_cast<ULONG>(in.size());
    NTSTATUS s = encrypt ? BCryptEncrypt(handle, input, length, &mode, nullptr, 0, out->data(),
                                         length, &done, 0)
                         : BCryptDecrypt(handle, input, length, &mode, nullptr, 0, out->data(),
                                         length, &done, 0);
    ok = s == 0 && done == length;
  }
  if (handle != nullptr) BCryptDestroyKey(handle);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "supported";
  HRESULT hr = S_OK;
  int supported = hello_key::Supported(&hr);
  printf("supported=%d hr=0x%08lx\n", supported, (unsigned long)hr);
  if (strcmp(mode, "supported") == 0) return supported >= 0 ? 0 : 1;
  if (supported != 1) {
    printf("Windows Hello is not set up for this user: nothing more to probe\n");
    return 2;
  }

  HWND w = MakeWindow();
  for (int i = 0; i < 20 && GetForegroundWindow() != w; i++) {
    Pump();
    Sleep(50);
  }
  printf("foreground is probe window: %s\n", GetForegroundWindow() == w ? "yes" : "no");

  std::wstring name = L"Gesher probe key " + std::to_wstring(GetTickCount());
  std::vector<uint8_t> publicKey;
  int status = Run(hello_key::StartCreate(GetCurrentProcessId(), name.data(), name.size()),
                   &publicKey, 0);
  printf("create: %d (%s), public key %zu bytes\n", status, StatusName(status),
         publicKey.size());
  if (status != hello_key::kSuccess) return 3;
  std::vector<uint8_t> opened;
  status = hello_key::Open(name.data(), name.size(), &opened, &hr);
  printf("open: %d (%s), same public key: %s\n", status, StatusName(status),
         opened == publicKey ? "yes" : "no");

  const std::vector<uint8_t> message = {'G', 'e', 's', 'h', 'e', 'r', ' ', 'p', 'r', 'o', 'b', 'e'};
  int exitCode = 0;
  if (strcmp(mode, "cancel") == 0) {
    SetForegroundWindow(w);
    std::vector<uint8_t> none;
    status = Run(hello_key::StartSign(GetCurrentProcessId(), name.data(), name.size(),
                                      message.data(), message.size()),
                 &none, 3000);
    printf("canceled sign: %d (%s), %zu bytes\n", status, StatusName(status), none.size());
  } else {
    std::vector<uint8_t> first, second;
    SetForegroundWindow(w);
    status = Run(hello_key::StartSign(GetCurrentProcessId(), name.data(), name.size(),
                                      message.data(), message.size()),
                 &first, 0);
    printf("sign 1: %d (%s), %zu bytes\n", status, StatusName(status), first.size());
    SetForegroundWindow(w);
    int status2 = Run(hello_key::StartSign(GetCurrentProcessId(), name.data(), name.size(),
                                           message.data(), message.size()),
                      &second, 0);
    printf("sign 2: %d (%s), %zu bytes\n", status2, StatusName(status2), second.size());
    if (status == hello_key::kSuccess && status2 == hello_key::kSuccess) {
      bool identical = first == second;
      printf("signatures identical: %s\n", identical ? "yes" : "NO");
      printf("verifies as PKCS#1 v1.5 SHA-256: %s\n",
             Verifies(publicKey, message, first, false) ? "yes" : "no");
      printf("verifies as PSS SHA-256 (salt 32): %s\n",
             Verifies(publicKey, message, first, true) ? "yes" : "no");
      // Wrap with the first signature, unwrap with the second.
      const std::vector<uint8_t> secret = {'t', 'e', 's', 't', ' ', 's', 'e', 'c', 'r', 'e', 't'};
      uint8_t key1[32], key2[32], tag[16];
      std::vector<uint8_t> wrapped, unwrapped;
      bool wrappedOk = Sha256(first, key1) && Gcm(true, key1, secret, &wrapped, tag);
      bool unwrappedOk = wrappedOk && Sha256(second, key2) &&
                         Gcm(false, key2, wrapped, &unwrapped, tag) && unwrapped == secret;
      printf("wrap with signature 1: %s; unwrap with signature 2: %s\n",
             wrappedOk ? "ok" : "FAILED", unwrappedOk ? "ok, secret recovered" : "FAILED");
      if (!identical || !unwrappedOk) exitCode = 4;
    } else {
      exitCode = 3;
    }
  }

  status = hello_key::Delete(name.data(), name.size(), &hr);
  printf("delete: %d (%s)\n", status, StatusName(status));
  status = hello_key::Open(name.data(), name.size(), &opened, &hr);
  printf("open after delete: %d (%s)\n", status, StatusName(status));
  DestroyWindow(w);
  return exitCode;
}
