// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows' WebAuthn API (webauthn.dll) for
// com.gesher.server.WindowsWebAuthn (docs/passkeys.md, ADR 0093). The DLL is
// loaded at run time from System32, so the runtime starts on Windows editions
// without it. Every result here is Windows' own: a credential or an assertion
// exists only when Windows returned S_OK with one.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace gesher::devices::webauthn {

// WEBAUTHN_CTAP_TRANSPORT_*.
constexpr DWORD kTransportUsb = 0x01, kTransportNfc = 0x02, kTransportBle = 0x04,
                kTransportInternal = 0x10, kTransportHybrid = 0x20, kTransportSmartCard = 0x40;
// WEBAUTHN_AUTHENTICATOR_ATTACHMENT_*.
constexpr DWORD kAttachmentAny = 0, kAttachmentPlatform = 1, kAttachmentCrossPlatform = 2;
// WEBAUTHN_USER_VERIFICATION_REQUIREMENT_*.
constexpr DWORD kUvRequired = 1, kUvPreferred = 2, kUvDiscouraged = 3;
// WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_*.
constexpr DWORD kAttestationNone = 1, kAttestationIndirect = 2, kAttestationDirect = 3;
// How a discoverable credential is asked for.
constexpr int kResidentKeyDiscouraged = 0, kResidentKeyPreferred = 1, kResidentKeyRequired = 2;

// What the functions below return when webauthn.dll, or the export a call
// needs, is not in this Windows.
constexpr HRESULT kUnavailable = E_NOTIMPL;
// What a ceremony returns, without calling Windows, when it has no owner window.
// HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE).
constexpr HRESULT kNoOwner = static_cast<HRESULT>(0x80070578L);

// WebAuthNGetApiVersionNumber; 0 without webauthn.dll.
DWORD ApiVersion();

// WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable (Windows Hello is set up).
HRESULT IsUserVerifyingPlatformAuthenticatorAvailable(bool* available);

// WebAuthNGetCancellationId: an id for one ceremony.
HRESULT GetCancellationId(GUID* id);

// WebAuthNCancelCurrentOperation: ends the ceremony that was started with `id`.
// Thread-safe; Windows then fails that ceremony as canceled.
HRESULT CancelCurrentOperation(const GUID& id);

// WebAuthNGetErrorName: the DOMException name Windows gives `hr` ("NotAllowedError",
// ...); empty without webauthn.dll.
std::wstring ErrorName(HRESULT hr);

// The foreground top-level window when process `pid` owns it, else null: the
// owner of Windows' dialog for a request of that process. A caller that is not
// in the foreground gets no dialog. The rule and the checks are hello_win.cc's
// (branch feat/biometric-hello-native) and clipboard_access.cc's.
HWND ForegroundWindowOf(DWORD pid);

struct Descriptor {
  std::vector<uint8_t> id;
  DWORD transports = 0;  // kTransport* bits; 0: any
};

struct MakeCredentialRequest {
  std::wstring rp_id;
  std::wstring rp_name;
  std::vector<uint8_t> user_id;
  std::wstring user_name;
  std::wstring user_display_name;
  std::vector<LONG> algorithms;           // COSE identifiers, most preferred first
  std::vector<uint8_t> client_data_json;  // UTF-8; Windows hashes it with SHA-256
  DWORD timeout_ms = 0;
  std::vector<Descriptor> exclude;
  DWORD attachment = kAttachmentAny;
  int resident_key = kResidentKeyDiscouraged;
  DWORD user_verification = kUvPreferred;
  DWORD attestation = kAttestationNone;
};

struct Attestation {
  std::vector<uint8_t> credential_id;
  std::vector<uint8_t> authenticator_data;
  std::vector<uint8_t> attestation_object;
  DWORD used_transport = 0;  // one kTransport* bit; 0 when Windows did not say
  int transports = -1;       // kTransport* bits; -1 when this Windows does not report them
  int resident_key = -1;     // bResidentKey; -1 when this Windows does not report it
};

// WebAuthNAuthenticatorMakeCredential with Windows' dialog owned by `owner`.
// Blocks until the user or Windows ends the ceremony. `cancellation` may be
// null. kNoOwner, and no call to Windows, when `owner` is null.
HRESULT MakeCredential(HWND owner, const MakeCredentialRequest& request, const GUID* cancellation,
                       Attestation* out);

struct GetAssertionRequest {
  std::wstring rp_id;
  std::vector<uint8_t> client_data_json;
  DWORD timeout_ms = 0;
  std::vector<Descriptor> allow;
  DWORD user_verification = kUvPreferred;
};

struct Assertion {
  std::vector<uint8_t> credential_id;
  std::vector<uint8_t> authenticator_data;
  std::vector<uint8_t> signature;
  std::vector<uint8_t> user_id;  // empty when the authenticator returned none
  DWORD used_transport = 0;
};

// WebAuthNAuthenticatorGetAssertion; as MakeCredential.
HRESULT GetAssertion(HWND owner, const GetAssertionRequest& request, const GUID* cancellation,
                     Assertion* out);

struct PlatformCredential {
  std::vector<uint8_t> id;
  std::wstring rp_id;
  std::wstring user_name;
  bool removable = false;
};

// WebAuthNGetPlatformCredentialList for `rp_id` (null: every relying party).
// Windows' NTE_NOT_FOUND, its answer when there are none, is S_OK and an empty
// list. kUnavailable before API version 4.
HRESULT PlatformCredentials(const wchar_t* rp_id, std::vector<PlatformCredential>* out);

// WebAuthNDeletePlatformCredential. kUnavailable before API version 4.
HRESULT DeletePlatformCredential(const std::vector<uint8_t>& id);

}  // namespace gesher::devices::webauthn
