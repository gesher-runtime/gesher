// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows' WebAuthn API through webauthn.dll's exports
// (docs/passkeys.md, ADR 0093).

#include "webauthn_win.h"

#include <stdio.h>

namespace gesher::devices::webauthn {

namespace {

// webauthn.dll's structures, by their ABI, as webauthn.h declares them
// (https://github.com/microsoft/webauthn/blob/master/webauthn.h, MIT License,
// Copyright (c) Microsoft Corporation). They are declared here because the
// toolchains' own webauthn.h differ in the API version they know (llvm-mingw's
// stops at 3). Each structure carries the version Gesher fills in or reads up
// to; Windows accepts older versions of its inputs and says in dwVersion how
// much of an output it filled.
namespace abi {

struct RpEntityInformation {  // WEBAUTHN_RP_ENTITY_INFORMATION, version 1
  DWORD dwVersion;
  PCWSTR pwszId;
  PCWSTR pwszName;
  PCWSTR pwszIcon;
};

struct UserEntityInformation {  // WEBAUTHN_USER_ENTITY_INFORMATION, version 1
  DWORD dwVersion;
  DWORD cbId;
  PBYTE pbId;
  PCWSTR pwszName;
  PCWSTR pwszIcon;
  PCWSTR pwszDisplayName;
};

struct ClientData {  // WEBAUTHN_CLIENT_DATA, version 1
  DWORD dwVersion;
  DWORD cbClientDataJSON;
  PBYTE pbClientDataJSON;
  LPCWSTR pwszHashAlgId;
};

struct CoseCredentialParameter {  // WEBAUTHN_COSE_CREDENTIAL_PARAMETER, version 1
  DWORD dwVersion;
  LPCWSTR pwszCredentialType;
  LONG lAlg;
};

struct CoseCredentialParameters {  // WEBAUTHN_COSE_CREDENTIAL_PARAMETERS
  DWORD cCredentialParameters;
  CoseCredentialParameter* pCredentialParameters;
};

struct Credential {  // WEBAUTHN_CREDENTIAL, version 1
  DWORD dwVersion;
  DWORD cbId;
  PBYTE pbId;
  LPCWSTR pwszCredentialType;
};

struct Credentials {  // WEBAUTHN_CREDENTIALS
  DWORD cCredentials;
  Credential* pCredentials;
};

struct CredentialEx {  // WEBAUTHN_CREDENTIAL_EX, version 1
  DWORD dwVersion;
  DWORD cbId;
  PBYTE pbId;
  LPCWSTR pwszCredentialType;
  DWORD dwTransports;
};

struct CredentialList {  // WEBAUTHN_CREDENTIAL_LIST
  DWORD cCredentials;
  CredentialEx** ppCredentials;
};

struct Extensions {  // WEBAUTHN_EXTENSIONS
  DWORD cExtensions;
  void* pExtensions;
};

// WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS through version 4.
struct MakeCredentialOptions {
  DWORD dwVersion;
  DWORD dwTimeoutMilliseconds;
  Credentials credential_list;  // CredentialList
  abi::Extensions extensions;  // Extensions
  DWORD dwAuthenticatorAttachment;
  BOOL bRequireResidentKey;
  DWORD dwUserVerificationRequirement;
  DWORD dwAttestationConveyancePreference;
  DWORD dwFlags;
  // Version 2.
  GUID* pCancellationId;
  // Version 3.
  abi::CredentialList* pExcludeCredentialList;
  // Version 4.
  DWORD dwEnterpriseAttestation;
  DWORD dwLargeBlobSupport;
  BOOL bPreferResidentKey;
};

// WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS through version 4.
struct GetAssertionOptions {
  DWORD dwVersion;
  DWORD dwTimeoutMilliseconds;
  Credentials credential_list;  // CredentialList
  abi::Extensions extensions;  // Extensions
  DWORD dwAuthenticatorAttachment;
  DWORD dwUserVerificationRequirement;
  DWORD dwFlags;
  // Version 2.
  PCWSTR pwszU2fAppId;
  BOOL* pbU2fAppId;
  // Version 3.
  GUID* pCancellationId;
  // Version 4.
  abi::CredentialList* pAllowCredentialList;
};

// WEBAUTHN_CREDENTIAL_ATTESTATION through version 8, up to dwTransports.
struct CredentialAttestation {
  DWORD dwVersion;
  PCWSTR pwszFormatType;
  DWORD cbAuthenticatorData;
  PBYTE pbAuthenticatorData;
  DWORD cbAttestation;
  PBYTE pbAttestation;
  DWORD dwAttestationDecodeType;
  PVOID pvAttestationDecode;
  DWORD cbAttestationObject;
  PBYTE pbAttestationObject;
  DWORD cbCredentialId;
  PBYTE pbCredentialId;
  // Version 2.
  abi::Extensions extensions;  // Extensions
  // Version 3.
  DWORD dwUsedTransport;
  // Version 4.
  BOOL bEpAtt;
  BOOL bLargeBlobSupported;
  BOOL bResidentKey;
  // Version 5.
  BOOL bPrfEnabled;
  // Version 6.
  DWORD cbUnsignedExtensionOutputs;
  PBYTE pbUnsignedExtensionOutputs;
  // Version 7.
  void* pHmacSecret;
  BOOL bThirdPartyPayment;
  // Version 8.
  DWORD dwTransports;
};

// WEBAUTHN_ASSERTION through version 4.
struct Assertion {
  DWORD dwVersion;
  DWORD cbAuthenticatorData;
  PBYTE pbAuthenticatorData;
  DWORD cbSignature;
  PBYTE pbSignature;
  abi::Credential credential;  // Credential
  DWORD cbUserId;
  PBYTE pbUserId;
  // Version 2.
  abi::Extensions extensions;  // Extensions
  DWORD cbCredLargeBlob;
  PBYTE pbCredLargeBlob;
  DWORD dwCredLargeBlobStatus;
  // Version 3.
  void* pHmacSecret;
  // Version 4.
  DWORD dwUsedTransport;
};

struct CredentialDetails {  // WEBAUTHN_CREDENTIAL_DETAILS, version 1
  DWORD dwVersion;
  DWORD cbCredentialID;
  PBYTE pbCredentialID;
  RpEntityInformation* pRpInformation;
  UserEntityInformation* pUserInformation;
  BOOL bRemovable;
};

struct CredentialDetailsList {  // WEBAUTHN_CREDENTIAL_DETAILS_LIST
  DWORD cCredentialDetails;
  CredentialDetails** ppCredentialDetails;
};

struct GetCredentialsOptions {  // WEBAUTHN_GET_CREDENTIALS_OPTIONS, version 1
  DWORD dwVersion;
  LPCWSTR pwszRpId;
  BOOL bBrowserInPrivateMode;
};

}  // namespace abi

constexpr wchar_t kPublicKey[] = L"public-key";  // WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY
constexpr wchar_t kSha256[] = L"SHA-256";        // WEBAUTHN_HASH_ALGORITHM_SHA_256
constexpr HRESULT kNteNotFound = static_cast<HRESULT>(0x80090011L);  // NTE_NOT_FOUND

struct Api {
  DWORD(WINAPI* GetApiVersionNumber)() = nullptr;
  HRESULT(WINAPI* IsUserVerifyingPlatformAuthenticatorAvailable)(BOOL*) = nullptr;
  HRESULT(WINAPI* AuthenticatorMakeCredential)(
      HWND, const abi::RpEntityInformation*, const abi::UserEntityInformation*,
      const abi::CoseCredentialParameters*, const abi::ClientData*,
      const abi::MakeCredentialOptions*, abi::CredentialAttestation**) = nullptr;
  HRESULT(WINAPI* AuthenticatorGetAssertion)(HWND, LPCWSTR, const abi::ClientData*,
                                             const abi::GetAssertionOptions*,
                                             abi::Assertion**) = nullptr;
  void(WINAPI* FreeCredentialAttestation)(abi::CredentialAttestation*) = nullptr;
  void(WINAPI* FreeAssertion)(abi::Assertion*) = nullptr;
  HRESULT(WINAPI* GetCancellationId)(GUID*) = nullptr;
  HRESULT(WINAPI* CancelCurrentOperation)(const GUID*) = nullptr;
  HRESULT(WINAPI* GetPlatformCredentialList)(const abi::GetCredentialsOptions*,
                                             abi::CredentialDetailsList**) = nullptr;
  void(WINAPI* FreePlatformCredentialList)(abi::CredentialDetailsList*) = nullptr;
  HRESULT(WINAPI* DeletePlatformCredential)(DWORD, const BYTE*) = nullptr;
  PCWSTR(WINAPI* GetErrorName)(HRESULT) = nullptr;
};

template <typename T>
void Bind(HMODULE dll, const char* name, T* function) {
  *function = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

Api Load() {
  Api api;
  // System32 only: an application's directory never supplies this DLL.
  HMODULE dll = LoadLibraryExW(L"webauthn.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (dll == nullptr) return api;
  Bind(dll, "WebAuthNGetApiVersionNumber", &api.GetApiVersionNumber);
  Bind(dll, "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable",
       &api.IsUserVerifyingPlatformAuthenticatorAvailable);
  Bind(dll, "WebAuthNAuthenticatorMakeCredential", &api.AuthenticatorMakeCredential);
  Bind(dll, "WebAuthNAuthenticatorGetAssertion", &api.AuthenticatorGetAssertion);
  Bind(dll, "WebAuthNFreeCredentialAttestation", &api.FreeCredentialAttestation);
  Bind(dll, "WebAuthNFreeAssertion", &api.FreeAssertion);
  Bind(dll, "WebAuthNGetCancellationId", &api.GetCancellationId);
  Bind(dll, "WebAuthNCancelCurrentOperation", &api.CancelCurrentOperation);
  Bind(dll, "WebAuthNGetPlatformCredentialList", &api.GetPlatformCredentialList);
  Bind(dll, "WebAuthNFreePlatformCredentialList", &api.FreePlatformCredentialList);
  Bind(dll, "WebAuthNDeletePlatformCredential", &api.DeletePlatformCredential);
  Bind(dll, "WebAuthNGetErrorName", &api.GetErrorName);
  return api;
}

const Api& GetApi() {
  static const Api api = Load();  // the DLL stays loaded for the process
  return api;
}

void Log(const char* call, HRESULT hr) {
  fprintf(stderr, "gesher webauthn: %s: 0x%08lx\n", call, static_cast<unsigned long>(hr));
}

std::vector<uint8_t> Bytes(const BYTE* data, DWORD size) {
  if (data == nullptr || size == 0) return std::vector<uint8_t>();
  return std::vector<uint8_t>(data, data + size);
}

BYTE* Data(const std::vector<uint8_t>& bytes) {
  return bytes.empty() ? nullptr : const_cast<BYTE*>(bytes.data());
}

// A WEBAUTHN_CREDENTIAL_LIST over `descriptors`, which must outlive it.
struct DescriptorList {
  std::vector<abi::CredentialEx> entries;
  std::vector<abi::CredentialEx*> pointers;
  abi::CredentialList list{};

  explicit DescriptorList(const std::vector<Descriptor>& descriptors) {
    entries.reserve(descriptors.size());
    for (const Descriptor& descriptor : descriptors) {
      abi::CredentialEx entry{};
      entry.dwVersion = 1;
      entry.cbId = static_cast<DWORD>(descriptor.id.size());
      entry.pbId = Data(descriptor.id);
      entry.pwszCredentialType = kPublicKey;
      entry.dwTransports = descriptor.transports;
      entries.push_back(entry);
    }
    for (abi::CredentialEx& entry : entries) pointers.push_back(&entry);
    list.cCredentials = static_cast<DWORD>(pointers.size());
    list.ppCredentials = pointers.empty() ? nullptr : pointers.data();
  }

  abi::CredentialList* get() { return pointers.empty() ? nullptr : &list; }
};

abi::ClientData ClientDataOf(const std::vector<uint8_t>& json) {
  abi::ClientData data{};
  data.dwVersion = 1;
  data.cbClientDataJSON = static_cast<DWORD>(json.size());
  data.pbClientDataJSON = Data(json);
  data.pwszHashAlgId = kSha256;
  return data;
}

}  // namespace

DWORD ApiVersion() {
  const Api& api = GetApi();
  return api.GetApiVersionNumber != nullptr ? api.GetApiVersionNumber() : 0;
}

HRESULT IsUserVerifyingPlatformAuthenticatorAvailable(bool* available) {
  *available = false;
  const Api& api = GetApi();
  if (api.IsUserVerifyingPlatformAuthenticatorAvailable == nullptr) return kUnavailable;
  BOOL value = FALSE;
  HRESULT hr = api.IsUserVerifyingPlatformAuthenticatorAvailable(&value);
  if (FAILED(hr)) {
    Log("WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable", hr);
    return hr;
  }
  *available = value != FALSE;
  return S_OK;
}

HRESULT GetCancellationId(GUID* id) {
  *id = GUID{};
  const Api& api = GetApi();
  if (api.GetCancellationId == nullptr) return kUnavailable;
  HRESULT hr = api.GetCancellationId(id);
  if (FAILED(hr)) Log("WebAuthNGetCancellationId", hr);
  return hr;
}

HRESULT CancelCurrentOperation(const GUID& id) {
  const Api& api = GetApi();
  if (api.CancelCurrentOperation == nullptr) return kUnavailable;
  HRESULT hr = api.CancelCurrentOperation(&id);
  if (FAILED(hr)) Log("WebAuthNCancelCurrentOperation", hr);
  return hr;
}

std::wstring ErrorName(HRESULT hr) {
  const Api& api = GetApi();
  if (api.GetErrorName == nullptr) return std::wstring();
  PCWSTR name = api.GetErrorName(hr);
  return name != nullptr ? std::wstring(name) : std::wstring();
}

HWND ForegroundWindowOf(DWORD pid) {
  HWND window = GetForegroundWindow();
  if (window == nullptr) return nullptr;
  DWORD owner = 0;
  DWORD thread = GetWindowThreadProcessId(window, &owner);
  if (thread == 0 || owner != pid) return nullptr;
  // A PID alone is not enough: hold the process while sampling, and reject an
  // exit or an activation change during the query (clipboard_access.cc).
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner);
  if (process == nullptr) return nullptr;
  DWORD current_owner = 0;
  bool valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
               GetWindowThreadProcessId(window, &current_owner) == thread &&
               current_owner == owner && GetForegroundWindow() == window;
  CloseHandle(process);
  if (!valid) return nullptr;
  HWND root = GetAncestor(window, GA_ROOTOWNER);
  return root != nullptr ? root : window;
}

HRESULT MakeCredential(HWND owner, const MakeCredentialRequest& request, const GUID* cancellation,
                       Attestation* out) {
  *out = Attestation();
  if (owner == nullptr) return kNoOwner;
  const Api& api = GetApi();
  DWORD version = ApiVersion();
  if (version == 0 || api.AuthenticatorMakeCredential == nullptr ||
      api.FreeCredentialAttestation == nullptr) {
    return kUnavailable;
  }

  abi::RpEntityInformation rp{};
  rp.dwVersion = 1;
  rp.pwszId = request.rp_id.c_str();
  rp.pwszName = request.rp_name.c_str();

  abi::UserEntityInformation user{};
  user.dwVersion = 1;
  user.cbId = static_cast<DWORD>(request.user_id.size());
  user.pbId = Data(request.user_id);
  user.pwszName = request.user_name.c_str();
  user.pwszDisplayName = request.user_display_name.c_str();

  std::vector<abi::CoseCredentialParameter> parameters;
  parameters.reserve(request.algorithms.size());
  for (LONG algorithm : request.algorithms) {
    abi::CoseCredentialParameter parameter{};
    parameter.dwVersion = 1;
    parameter.pwszCredentialType = kPublicKey;
    parameter.lAlg = algorithm;
    parameters.push_back(parameter);
  }
  abi::CoseCredentialParameters cose{};
  cose.cCredentialParameters = static_cast<DWORD>(parameters.size());
  cose.pCredentialParameters = parameters.empty() ? nullptr : parameters.data();

  abi::ClientData client = ClientDataOf(request.client_data_json);
  DescriptorList exclude(request.exclude);
  GUID cancellation_id = cancellation != nullptr ? *cancellation : GUID{};

  abi::MakeCredentialOptions options{};
  // API version 3 added options version 4, whose bPreferResidentKey expresses
  // residentKey "preferred". Before it, a preference is not a requirement.
  options.dwVersion = version >= 3 ? 4 : 3;
  options.dwTimeoutMilliseconds = request.timeout_ms;
  options.dwAuthenticatorAttachment = request.attachment;
  options.bRequireResidentKey = request.resident_key == kResidentKeyRequired;
  options.dwUserVerificationRequirement = request.user_verification;
  options.dwAttestationConveyancePreference = request.attestation;
  options.pCancellationId = cancellation != nullptr ? &cancellation_id : nullptr;
  options.pExcludeCredentialList = exclude.get();
  options.bPreferResidentKey = request.resident_key == kResidentKeyPreferred;

  abi::CredentialAttestation* attestation = nullptr;
  HRESULT hr = api.AuthenticatorMakeCredential(owner, &rp, &user, &cose, &client, &options,
                                               &attestation);
  if (SUCCEEDED(hr) && attestation == nullptr) hr = E_UNEXPECTED;
  if (FAILED(hr)) {
    Log("WebAuthNAuthenticatorMakeCredential", hr);
    return hr;
  }
  out->credential_id = Bytes(attestation->pbCredentialId, attestation->cbCredentialId);
  out->authenticator_data =
      Bytes(attestation->pbAuthenticatorData, attestation->cbAuthenticatorData);
  out->attestation_object =
      Bytes(attestation->pbAttestationObject, attestation->cbAttestationObject);
  if (attestation->dwVersion >= 3) out->used_transport = attestation->dwUsedTransport;
  if (attestation->dwVersion >= 4) out->resident_key = attestation->bResidentKey ? 1 : 0;
  if (attestation->dwVersion >= 8) out->transports = static_cast<int>(attestation->dwTransports);
  api.FreeCredentialAttestation(attestation);
  return S_OK;
}

HRESULT GetAssertion(HWND owner, const GetAssertionRequest& request, const GUID* cancellation,
                     Assertion* out) {
  *out = Assertion();
  if (owner == nullptr) return kNoOwner;
  const Api& api = GetApi();
  if (ApiVersion() == 0 || api.AuthenticatorGetAssertion == nullptr ||
      api.FreeAssertion == nullptr) {
    return kUnavailable;
  }

  abi::ClientData client = ClientDataOf(request.client_data_json);
  DescriptorList allow(request.allow);
  GUID cancellation_id = cancellation != nullptr ? *cancellation : GUID{};

  abi::GetAssertionOptions options{};
  options.dwVersion = 4;  // API version 1 has it: cancellation and the allow list
  options.dwTimeoutMilliseconds = request.timeout_ms;
  options.dwUserVerificationRequirement = request.user_verification;
  options.pCancellationId = cancellation != nullptr ? &cancellation_id : nullptr;
  options.pAllowCredentialList = allow.get();

  abi::Assertion* assertion = nullptr;
  HRESULT hr = api.AuthenticatorGetAssertion(owner, request.rp_id.c_str(), &client, &options,
                                             &assertion);
  if (SUCCEEDED(hr) && assertion == nullptr) hr = E_UNEXPECTED;
  if (FAILED(hr)) {
    Log("WebAuthNAuthenticatorGetAssertion", hr);
    return hr;
  }
  out->credential_id = Bytes(assertion->credential.pbId, assertion->credential.cbId);
  out->authenticator_data = Bytes(assertion->pbAuthenticatorData, assertion->cbAuthenticatorData);
  out->signature = Bytes(assertion->pbSignature, assertion->cbSignature);
  out->user_id = Bytes(assertion->pbUserId, assertion->cbUserId);
  if (assertion->dwVersion >= 4) out->used_transport = assertion->dwUsedTransport;
  api.FreeAssertion(assertion);
  return S_OK;
}

HRESULT PlatformCredentials(const wchar_t* rp_id, std::vector<PlatformCredential>* out) {
  out->clear();
  const Api& api = GetApi();
  if (api.GetPlatformCredentialList == nullptr || api.FreePlatformCredentialList == nullptr) {
    return kUnavailable;
  }
  abi::GetCredentialsOptions options{};
  options.dwVersion = 1;
  options.pwszRpId = rp_id;
  abi::CredentialDetailsList* list = nullptr;
  HRESULT hr = api.GetPlatformCredentialList(&options, &list);
  if (hr == kNteNotFound) return S_OK;
  if (FAILED(hr)) {
    Log("WebAuthNGetPlatformCredentialList", hr);
    return hr;
  }
  if (list == nullptr) return S_OK;
  for (DWORD i = 0; i < list->cCredentialDetails; i++) {
    const abi::CredentialDetails* details = list->ppCredentialDetails[i];
    if (details == nullptr) continue;
    PlatformCredential credential;
    credential.id = Bytes(details->pbCredentialID, details->cbCredentialID);
    if (details->pRpInformation != nullptr && details->pRpInformation->pwszId != nullptr) {
      credential.rp_id = details->pRpInformation->pwszId;
    }
    if (details->pUserInformation != nullptr && details->pUserInformation->pwszName != nullptr) {
      credential.user_name = details->pUserInformation->pwszName;
    }
    credential.removable = details->bRemovable != FALSE;
    out->push_back(credential);
  }
  api.FreePlatformCredentialList(list);
  return S_OK;
}

HRESULT DeletePlatformCredential(const std::vector<uint8_t>& id) {
  const Api& api = GetApi();
  if (api.DeletePlatformCredential == nullptr) return kUnavailable;
  HRESULT hr = api.DeletePlatformCredential(static_cast<DWORD>(id.size()), id.data());
  if (FAILED(hr)) Log("WebAuthNDeletePlatformCredential", hr);
  return hr;
}

}  // namespace gesher::devices::webauthn
