// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Manual probe for compat/devices/webauthn_win.cc (docs/passkeys.md, ADR 0092): the real
// Windows WebAuthn ceremonies, which need a person. See tests/credentials/HUMAN_TEST.md.
//
//   webauthn_probe info      API version and platform authenticator, no UI
//   webauthn_probe           make a credential for gesher-test.example with Windows Hello,
//                            get an assertion for it, verify the assertion's signature with
//                            the credential's COSE public key, then delete the credential
//   webauthn_probe cancel    start a registration and cancel it after 3 seconds through
//                            its cancellation id, as an application's cancel does
//   webauthn_probe host [seconds]   show a window (60 s by default) and print its pid:
//                                   the "app" of the mode below
//   webauthn_probe foreign <pid>    this process (the "system server") runs the round
//                                   trip with the foreground window of process <pid> as
//                                   the owner: the cross-process owner Gesher uses
//
// The probe is the production native code plus BCrypt for the verification. It is not an
// Android application, so its clientDataJSON origin is a label of its own.

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <thread>
#include <vector>

#include "webauthn_win.h"

using namespace gesher::devices;

namespace {

typedef std::vector<uint8_t> Bytes;

constexpr wchar_t kRpId[] = L"gesher-test.example";
constexpr char kRpIdUtf8[] = "gesher-test.example";

unsigned long Hex(HRESULT hr) { return static_cast<unsigned long>(hr); }

bool Sha256(const Bytes& data, Bytes* digest) {
  digest->assign(32, 0);
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    return false;
  }
  NTSTATUS status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data.data()),
                               static_cast<ULONG>(data.size()), digest->data(), 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status == 0;
}

std::string Base64Url(const Bytes& data) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    unsigned value = static_cast<unsigned>(data[i]) << 16 |
                     static_cast<unsigned>(data[i + 1]) << 8 | data[i + 2];
    out += kAlphabet[value >> 18];
    out += kAlphabet[(value >> 12) & 63];
    out += kAlphabet[(value >> 6) & 63];
    out += kAlphabet[value & 63];
  }
  if (i + 1 == data.size()) {
    unsigned value = static_cast<unsigned>(data[i]) << 16;
    out += kAlphabet[value >> 18];
    out += kAlphabet[(value >> 12) & 63];
  } else if (i + 2 == data.size()) {
    unsigned value = static_cast<unsigned>(data[i]) << 16 | static_cast<unsigned>(data[i + 1]) << 8;
    out += kAlphabet[value >> 18];
    out += kAlphabet[(value >> 12) & 63];
    out += kAlphabet[(value >> 6) & 63];
  }
  return out;
}

std::string HexOf(const Bytes& data, size_t limit) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < data.size() && i < limit; i++) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  if (data.size() > limit) out += "...";
  return out;
}

Bytes ClientData(const char* type, const Bytes& challenge) {
  std::string json = std::string("{\"type\":\"") + type + "\",\"challenge\":\"" +
                     Base64Url(challenge) +
                     "\",\"origin\":\"gesher-webauthn-probe\",\"crossOrigin\":false}";
  return Bytes(json.begin(), json.end());
}

Bytes Random(ULONG size) {
  Bytes bytes(size);
  BCryptGenRandom(nullptr, bytes.data(), size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return bytes;
}

// The credential public key of authenticator data: a COSE_Key of integer labels whose
// values are integers or byte strings (RFC 8949 definite lengths), as CTAP2 encodes it.
struct CoseKey {
  long long type = 0, algorithm = 0, curve = 0;  // labels 1, 3, -1 (EC2)
  Bytes x, y;                                    // labels -2, -3 (EC2)
  Bytes n, e;                                    // labels -1, -2 (RSA)
};

class CborReader {
 public:
  CborReader(const Bytes& data, size_t at) : data_(data), at_(at) {}

  bool Head(int* major, unsigned long long* argument) {
    if (at_ >= data_.size()) return false;
    uint8_t head = data_[at_++];
    *major = head >> 5;
    int info = head & 0x1f;
    if (info < 24) {
      *argument = static_cast<unsigned long long>(info);
      return true;
    }
    if (info > 27) return false;
    size_t count = static_cast<size_t>(1) << (info - 24);
    if (data_.size() - at_ < count) return false;
    *argument = 0;
    for (size_t i = 0; i < count; i++) *argument = *argument << 8 | data_[at_++];
    return true;
  }

  bool Integer(long long* value) {
    int major = 0;
    unsigned long long argument = 0;
    if (!Head(&major, &argument) || major > 1) return false;
    *value = major == 0 ? static_cast<long long>(argument) : -1 - static_cast<long long>(argument);
    return true;
  }

  // An integer (into *integer) or a byte string (into *bytes).
  bool Value(bool* is_integer, long long* integer, Bytes* bytes) {
    int major = 0;
    unsigned long long argument = 0;
    if (!Head(&major, &argument)) return false;
    if (major <= 1) {
      *is_integer = true;
      *integer = major == 0 ? static_cast<long long>(argument) : -1 - static_cast<long long>(argument);
      return true;
    }
    if (major != 2 || data_.size() - at_ < argument) return false;
    *is_integer = false;
    bytes->assign(data_.begin() + static_cast<ptrdiff_t>(at_),
                  data_.begin() + static_cast<ptrdiff_t>(at_ + static_cast<size_t>(argument)));
    at_ += static_cast<size_t>(argument);
    return true;
  }

 private:
  const Bytes& data_;
  size_t at_;
};

bool ParseCredentialKey(const Bytes& authenticator_data, CoseKey* key) {
  // rpIdHash 32, flags 1, signCount 4, aaguid 16, credentialIdLength 2, credentialId.
  if (authenticator_data.size() < 55 || (authenticator_data[32] & 0x40) == 0) return false;
  size_t id_length = static_cast<size_t>(authenticator_data[53]) << 8 | authenticator_data[54];
  CborReader reader(authenticator_data, 55 + id_length);
  int major = 0;
  unsigned long long count = 0;
  if (!reader.Head(&major, &count) || major != 5) return false;
  bool has_type = false;
  struct Pending {
    long long label;
    bool is_integer;
    long long integer;
    Bytes bytes;
  };
  std::vector<Pending> members;
  for (unsigned long long i = 0; i < count; i++) {
    Pending member{};
    if (!reader.Integer(&member.label) ||
        !reader.Value(&member.is_integer, &member.integer, &member.bytes)) {
      return false;
    }
    if (member.label == 1 && member.is_integer) {
      key->type = member.integer;
      has_type = true;
    }
    if (member.label == 3 && member.is_integer) key->algorithm = member.integer;
    members.push_back(member);
  }
  if (!has_type) return false;
  for (const Pending& member : members) {
    if (key->type == 2) {
      if (member.label == -1 && member.is_integer) key->curve = member.integer;
      if (member.label == -2 && !member.is_integer) key->x = member.bytes;
      if (member.label == -3 && !member.is_integer) key->y = member.bytes;
    } else if (key->type == 3) {
      if (member.label == -1 && !member.is_integer) key->n = member.bytes;
      if (member.label == -2 && !member.is_integer) key->e = member.bytes;
    }
  }
  return true;
}

// An ASN.1 ECDSA-Sig-Value as the fixed r || s that BCrypt verifies.
bool RawEcdsaSignature(const Bytes& der, size_t size, Bytes* raw) {
  raw->assign(size * 2, 0);
  size_t at = 0;
  if (der.size() < 8 || der[at++] != 0x30) return false;
  if (der[at] & 0x80) at += static_cast<size_t>(der[at] & 0x7f);
  at++;
  for (int part = 0; part < 2; part++) {
    if (at + 2 > der.size() || der[at] != 0x02) return false;
    size_t length = der[at + 1];
    at += 2;
    if (length > der.size() - at) return false;
    size_t skip = 0;
    while (skip < length && length - skip > size && der[at + skip] == 0) skip++;
    if (length - skip > size) return false;
    memcpy(raw->data() + static_cast<size_t>(part) * size + (size - (length - skip)),
           der.data() + at + skip, length - skip);
    at += length;
  }
  return true;
}

// Whether `signature` is the credential key's over authenticatorData || SHA-256(clientDataJSON).
bool VerifyAssertion(const CoseKey& key, const Bytes& authenticator_data,
                     const Bytes& client_data_json, const Bytes& signature) {
  Bytes client_hash, digest;
  if (!Sha256(client_data_json, &client_hash)) return false;
  Bytes signed_data = authenticator_data;
  signed_data.insert(signed_data.end(), client_hash.begin(), client_hash.end());
  if (!Sha256(signed_data, &digest)) return false;

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE handle = nullptr;
  NTSTATUS status = -1;
  if (key.type == 2 && key.algorithm == -7 && key.curve == 1 && key.x.size() == 32 &&
      key.y.size() == 32) {
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;
    memcpy(blob.data(), &header, sizeof(header));
    blob.insert(blob.end(), key.x.begin(), key.x.end());
    blob.insert(blob.end(), key.y.begin(), key.y.end());
    Bytes raw;
    if (!RawEcdsaSignature(signature, 32, &raw)) return false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
      return false;
    }
    status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &handle, blob.data(),
                                 static_cast<ULONG>(blob.size()), 0);
    if (status == 0) {
      status = BCryptVerifySignature(handle, nullptr, digest.data(), 32, raw.data(),
                                     static_cast<ULONG>(raw.size()), 0);
    }
  } else if (key.type == 3 && key.algorithm == -257 && !key.n.empty() && !key.e.empty()) {
    Bytes blob(sizeof(BCRYPT_RSAKEY_BLOB));
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = static_cast<ULONG>(key.n.size() * 8);
    header.cbPublicExp = static_cast<ULONG>(key.e.size());
    header.cbModulus = static_cast<ULONG>(key.n.size());
    memcpy(blob.data(), &header, sizeof(header));
    blob.insert(blob.end(), key.e.begin(), key.e.end());
    blob.insert(blob.end(), key.n.begin(), key.n.end());
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
      return false;
    }
    status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &handle, blob.data(),
                                 static_cast<ULONG>(blob.size()), 0);
    if (status == 0) {
      BCRYPT_PKCS1_PADDING_INFO padding{};
      padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
      status = BCryptVerifySignature(handle, &padding, digest.data(), 32,
                                     const_cast<PUCHAR>(signature.data()),
                                     static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1);
    }
  } else {
    printf("FAIL the credential key (COSE type %lld, algorithm %lld) is not ES256 or RS256\n",
           key.type, key.algorithm);
    return false;
  }
  if (handle != nullptr) BCryptDestroyKey(handle);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status == 0;
}

int failures;

void Check(bool ok, const char* what) {
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  fflush(stdout);
}

void PrintInfo() {
  printf("WebAuthNGetApiVersionNumber: %lu\n", static_cast<unsigned long>(webauthn::ApiVersion()));
  bool available = false;
  HRESULT hr = webauthn::IsUserVerifyingPlatformAuthenticatorAvailable(&available);
  printf("user-verifying platform authenticator: hr 0x%08lx, available %d\n", Hex(hr),
         available ? 1 : 0);
}

webauthn::MakeCredentialRequest Registration(const Bytes& client_data) {
  webauthn::MakeCredentialRequest request;
  request.rp_id = kRpId;
  request.rp_name = L"Gesher WebAuthn probe";
  request.user_id = Random(16);
  request.user_name = L"probe@gesher-test.example";
  request.user_display_name = L"Gesher probe";
  request.algorithms = {-7, -257};
  request.client_data_json = client_data;
  request.timeout_ms = 120000;
  request.attachment = webauthn::kAttachmentPlatform;
  request.resident_key = webauthn::kResidentKeyRequired;
  request.user_verification = webauthn::kUvRequired;
  request.attestation = webauthn::kAttestationNone;
  return request;
}

bool Listed(const Bytes& id, size_t* count) {
  std::vector<webauthn::PlatformCredential> credentials;
  HRESULT hr = webauthn::PlatformCredentials(kRpId, &credentials);
  *count = credentials.size();
  if (hr != S_OK) {
    printf("WebAuthNGetPlatformCredentialList: 0x%08lx\n", Hex(hr));
    return false;
  }
  for (const webauthn::PlatformCredential& credential : credentials) {
    if (credential.id == id) return true;
  }
  return false;
}

int RoundTrip(HWND owner) {
  Bytes expected_rp_hash;
  Sha256(Bytes(kRpIdUtf8, kRpIdUtf8 + sizeof(kRpIdUtf8) - 1), &expected_rp_hash);

  printf("\n1. Registration: confirm with Windows Hello in the dialog that opens.\n");
  Bytes create_data = ClientData("webauthn.create", Random(32));
  webauthn::MakeCredentialRequest registration = Registration(create_data);
  webauthn::Attestation attestation;
  HRESULT hr = webauthn::MakeCredential(owner, registration, nullptr, &attestation);
  printf("WebAuthNAuthenticatorMakeCredential: 0x%08lx (%ls)\n", Hex(hr),
         webauthn::ErrorName(hr).c_str());
  Check(hr == S_OK, "a credential was made");
  if (hr != S_OK) return 1;
  printf("credential id (%u bytes): %s\n", static_cast<unsigned>(attestation.credential_id.size()),
         HexOf(attestation.credential_id, 16).c_str());
  printf("used transport 0x%lx, transports %d, resident key %d, attestation object %u bytes\n",
         static_cast<unsigned long>(attestation.used_transport), attestation.transports,
         attestation.resident_key, static_cast<unsigned>(attestation.attestation_object.size()));
  const Bytes& made = attestation.authenticator_data;
  Check(made.size() >= 37 && memcmp(made.data(), expected_rp_hash.data(), 32) == 0,
        "authenticator data starts with SHA-256(gesher-test.example)");
  Check(made.size() >= 37 && (made[32] & 0x45) == 0x45,
        "flags: user present, user verified, attested credential data");
  Check(attestation.used_transport == webauthn::kTransportInternal,
        "the internal transport was used (Windows Hello)");
  CoseKey key;
  bool parsed = ParseCredentialKey(made, &key);
  Check(parsed, "the credential's COSE public key is read from the authenticator data");
  if (!parsed) return 1;
  printf("COSE key type %lld, algorithm %lld\n", key.type, key.algorithm);

  printf("\n2. Authentication: confirm with Windows Hello again.\n");
  Bytes get_data = ClientData("webauthn.get", Random(32));
  webauthn::GetAssertionRequest authentication;
  authentication.rp_id = kRpId;
  authentication.client_data_json = get_data;
  authentication.timeout_ms = 120000;
  authentication.user_verification = webauthn::kUvRequired;
  webauthn::Descriptor descriptor;
  descriptor.id = attestation.credential_id;
  descriptor.transports = webauthn::kTransportInternal;
  authentication.allow.push_back(descriptor);
  webauthn::Assertion assertion;
  hr = webauthn::GetAssertion(owner, authentication, nullptr, &assertion);
  printf("WebAuthNAuthenticatorGetAssertion: 0x%08lx (%ls)\n", Hex(hr),
         webauthn::ErrorName(hr).c_str());
  Check(hr == S_OK, "an assertion was returned");
  if (hr == S_OK) {
    Check(assertion.credential_id == attestation.credential_id,
          "the assertion is for the credential just made");
    Check(assertion.user_id == registration.user_id, "the user handle is the registered user id");
    Check(assertion.authenticator_data.size() >= 37 &&
              memcmp(assertion.authenticator_data.data(), expected_rp_hash.data(), 32) == 0 &&
              (assertion.authenticator_data[32] & 0x05) == 0x05,
          "assertion authenticator data: the relying party's hash, user present and verified");
    Check(VerifyAssertion(key, assertion.authenticator_data, get_data, assertion.signature),
          "the signature verifies with the COSE public key over "
          "authenticatorData || SHA-256(clientDataJSON)");
    Bytes other = ClientData("webauthn.get", Random(32));
    Check(!VerifyAssertion(key, assertion.authenticator_data, other, assertion.signature),
          "the signature does not verify for another challenge");
  }

  printf("\n3. Clean-up: the credential is deleted from Windows Hello.\n");
  size_t count = 0;
  Check(Listed(attestation.credential_id, &count), "the credential is in the platform list");
  hr = webauthn::DeletePlatformCredential(attestation.credential_id);
  printf("WebAuthNDeletePlatformCredential: 0x%08lx\n", Hex(hr));
  Check(hr == S_OK, "the credential was deleted");
  Check(!Listed(attestation.credential_id, &count), "the credential is no longer listed");
  printf("credentials left for gesher-test.example: %u\n", static_cast<unsigned>(count));
  return failures == 0 ? 0 : 1;
}

int Cancel(HWND owner) {
  GUID id{};
  HRESULT hr = webauthn::GetCancellationId(&id);
  Check(hr == S_OK, "a cancellation id was issued");
  if (hr != S_OK) return 1;
  printf("A registration dialog opens. Do not touch it: it is canceled after 3 seconds.\n");
  std::thread canceler([id] {
    Sleep(3000);
    HRESULT cancel = webauthn::CancelCurrentOperation(id);
    printf("WebAuthNCancelCurrentOperation: 0x%08lx\n", Hex(cancel));
  });
  webauthn::Attestation attestation;
  ULONGLONG start = GetTickCount64();
  hr = webauthn::MakeCredential(owner, Registration(ClientData("webauthn.create", Random(32))),
                                &id, &attestation);
  ULONGLONG elapsed = GetTickCount64() - start;
  canceler.join();
  printf("WebAuthNAuthenticatorMakeCredential: 0x%08lx (%ls) after %llu ms\n", Hex(hr),
         webauthn::ErrorName(hr).c_str(), elapsed);
  Check(FAILED(hr) && attestation.credential_id.empty(), "the canceled ceremony made no credential");
  Check(hr == static_cast<HRESULT>(0x80090036L) || hr == static_cast<HRESULT>(0x800704C7L),
        "Windows reports the cancel as NTE_USER_CANCELLED or ERROR_CANCELLED");
  Check(elapsed < 10000, "the dialog closed promptly after the cancel");
  size_t count = 0;
  Listed(Bytes(), &count);
  printf("credentials for gesher-test.example: %u\n", static_cast<unsigned>(count));
  return failures == 0 ? 0 : 1;
}

void Pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}

// A window of this process, standing in for an application's window.
int Host(DWORD seconds) {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"GesherWebAuthnProbe";
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(0, window_class.lpszClassName,
                                L"Gesher WebAuthn probe: app window",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                480, 240, nullptr, nullptr, window_class.hInstance, nullptr);
  SetForegroundWindow(window);
  printf("host pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
  fflush(stdout);
  for (ULONGLONG end = GetTickCount64() + seconds * 1000ULL; GetTickCount64() < end;) {
    Pump();
    Sleep(20);
  }
  return 0;
}

// The production owner rule: the foreground window of `pid`, or none.
HWND WaitForForegroundWindowOf(DWORD pid) {
  printf("Click the host window (pid %lu) within 15 seconds.\n", static_cast<unsigned long>(pid));
  fflush(stdout);
  for (ULONGLONG end = GetTickCount64() + 15000; GetTickCount64() < end; Sleep(100)) {
    HWND owner = webauthn::ForegroundWindowOf(pid);
    if (owner != nullptr) return owner;
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "roundtrip";
  if (strcmp(mode, "host") == 0) {
    return Host(argc > 2 ? strtoul(argv[2], nullptr, 10) : 60);
  }
  PrintInfo();
  if (strcmp(mode, "info") == 0) return 0;
  if (webauthn::ApiVersion() == 0) {
    printf("FAIL this Windows has no WebAuthn API (webauthn.dll)\n");
    return 1;
  }
  // The dialog's owner: the window the probe was started from (its terminal), or, as in
  // Gesher, another process's foreground window.
  bool foreign = strcmp(mode, "foreign") == 0;
  HWND owner = foreign ? WaitForForegroundWindowOf(argc > 2 ? strtoul(argv[2], nullptr, 10) : 0)
                       : GetForegroundWindow();
  if (foreign && owner == nullptr) {
    webauthn::Attestation none;
    HRESULT refused = webauthn::MakeCredential(nullptr, Registration(Bytes()), nullptr, &none);
    printf("that process does not own the foreground window: no dialog (0x%08lx)\n", Hex(refused));
    return 1;
  }
  wchar_t title[128] = L"";
  if (owner != nullptr) GetWindowTextW(owner, title, 128);
  printf("owner window: %p \"%ls\"\n", static_cast<void*>(owner), title);
  if (owner == nullptr) {
    printf("FAIL there is no foreground window to own the dialog\n");
    return 1;
  }
  int result = 0;
  if (strcmp(mode, "cancel") == 0) {
    result = Cancel(owner);
  } else if (strcmp(mode, "roundtrip") == 0 || foreign) {
    if (foreign) printf("the owner is another process's window\n");
    result = RoundTrip(owner);
  } else {
    printf("usage: webauthn_probe [info|roundtrip|cancel|host [seconds]|foreign <pid>]\n");
    return 2;
  }
  printf("\nwebauthn probe: %s\n", result == 0 ? "PASSED" : "FAILED");
  return result;
}
