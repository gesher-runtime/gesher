// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows Hello keys through their ABI
// (Windows.Security.Credentials.KeyCredentialManager and KeyCredential), for
// com.gesher.server.WindowsHelloKey (ADR 0092). A key lives in Windows under a
// name; a signature is whatever Windows returns after the user's Windows Hello
// gesture. Nothing here signs, or reports Success, on its own.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <vector>

namespace gesher::devices::hello_key {

// Status values are Windows' KeyCredentialStatus: Success 0, UnknownError 1,
// NotFound 2, UserCanceled 3, UserPrefersPassword 4, CredentialAlreadyExists 5,
// SecurityDeviceLocked 6 (7 AlgorithmNotSupported in newer SDKs), or:
constexpr int kSuccess = 0;
constexpr int kFailed = -1;         // a Windows call failed (logged with its HRESULT)
constexpr int kNotForeground = -2;  // `pid` does not own the foreground window

// KeyCredentialManager.IsSupportedAsync: 1, 0, or -1 when the query failed.
int Supported(HRESULT* hr);

// KeyCredentialManager.OpenAsync(name) and the key's public key
// (X.509 SubjectPublicKeyInfo). Shows no UI; waits at most 5 s.
int Open(const wchar_t* name, size_t length, std::vector<uint8_t>* publicKey, HRESULT* hr);

// KeyCredentialManager.DeleteAsync(name). Shows no UI; waits at most 5 s.
int Delete(const wchar_t* name, size_t length, HRESULT* hr);

// A user-paced request. Start* never returns null: a request that could not
// start is an operation whose Await returns the reason at once.
struct Operation;

// KeyCredentialManager.RequestCreateAsync(name, FailIfExists) for the
// foreground window of process `pid`. Await's bytes: the public key.
Operation* StartCreate(DWORD pid, const wchar_t* name, size_t length);

// KeyCredential.RequestSignAsync(data) with the key `name`, for the foreground
// window of process `pid`. Await's bytes: the signature.
Operation* StartSign(DWORD pid, const wchar_t* name, size_t length, const uint8_t* data,
                     size_t size);

// How long Await keeps waiting for Windows to end a prompt after Cancel.
constexpr DWORD kCancelGraceMs = 5000;

// Blocks until Windows' prompt ends. A status as above; with kSuccess, *bytes.
int Await(Operation* operation, std::vector<uint8_t>* bytes,
          DWORD cancel_grace_ms = kCancelGraceMs);

// Thread-safe and idempotent: asks Windows to close the prompt.
void Cancel(Operation* operation);

// Releases the operation once Await has returned.
void Release(Operation* operation);

namespace internal {

// Takes ownership of a started RequestCreateAsync (`sign` false) or
// RequestSignAsync operation (releases it on failure, and then returns an
// operation whose Await is kFailed). StartCreate and StartSign use it;
// hello_key_windows_test.cc passes scripted operations to exercise completion
// and cancellation without UI.
Operation* Wrap(bool sign, IUnknown* async);

}  // namespace internal

}  // namespace gesher::devices::hello_key
