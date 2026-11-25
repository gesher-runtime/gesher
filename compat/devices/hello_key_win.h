// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows Hello keys through their ABI
// (Windows.Security.Credentials.KeyCredentialManager and KeyCredential), for
// com.gesher.server.WindowsHelloKey (ADR 0095). A key lives in Windows under a
// name; a signature is whatever Windows returns after the user's Windows Hello
// gesture. Nothing here signs, or reports Success, on its own.
#pragma once

#include <windows.h>
#include <unknwn.h>
#include <stdint.h>

#include <vector>
#include <functional>

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
//
// After Cancel the wait is bounded: when Windows has not ended the request
// within `cancel_grace_ms` (its Cancel failed, or it ignores the request), the
// request is abandoned and Await returns UserCanceled (3) with no bytes. The
// caller then calls Release as usual, which drops Gesher's references; a
// late signature is never read. A creation keeps its single reserved cleanup
// slot until Windows completes; a successfully created but rejected key is
// deleted before another creation can start.
int Await(Operation* operation, std::vector<uint8_t>* bytes,
          DWORD cancel_grace_ms = kCancelGraceMs);

// Thread-safe and idempotent: asks Windows to close the prompt and starts
// Await's grace period, also when Windows refuses the request.
void Cancel(Operation* operation);

// Releases the caller's operation once Await has returned. For a creation,
// accept_result is true only if the public key was delivered to the caller and
// the caller accepted it. A canceled or rejected successful create is deleted.
void Release(Operation* operation, bool accept_result = true);

namespace internal {

// Takes ownership of a started signature operation (releases it on failure,
// then returns an operation whose Await is kFailed). StartSign uses it;
// hello_key_windows_test.cc also exercises the legacy retrieval-result ABI.
// Production creation uses the retained owner below, including its cleanup.
Operation* Wrap(bool sign, IUnknown* async);

// Production StartCreate uses this boundary. Capacity is reserved before start
// is called, and exactly one create/cleanup may be pending. start transfers one
// async reference; delete_created is called only after observed creation
// SUCCESS, with the exact name passed to start. A failed deletion is retried
// while the slot remains occupied. Tests supply scripted COM operations here.
Operation* StartCreate(
    const wchar_t* name, size_t length,
    const std::function<HRESULT(IUnknown**)>& start,
    const std::function<int(const wchar_t*, size_t, HRESULT*)>& delete_created);

// The actual cleanup action observer, with only DeleteAsync's start injected.
// A started action is retained across QI/status observation failures, until its
// terminal status is known. GetResults is called once, then the action is
// closed and released. A failed start or terminal action may be retried by the
// bounded create owner; an unobserved running action is never replaced.
int DeleteCreated(
    const wchar_t* name, size_t length, HRESULT* hr,
    const std::function<HRESULT(const wchar_t*, size_t, IUnknown**)>& start_delete);

}  // namespace internal

}  // namespace gesher::devices::hello_key
