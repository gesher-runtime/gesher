// Gesher: the system's trusted CA certificates from Windows' certificate store
// (trust_store.h; docs/network.md, ADR 0037).
//
// Android trusts the CA certificates in /system/etc/security/cacerts (or the
// Conscrypt APEX's copy): PEM files named by the OpenSSL "old" hash of their
// subject, <hash>.<n>, which Conscrypt's TrustedCertificateStore and the
// framework's network security config look up by issuer. Gesher writes that
// directory, under $ANDROID_ROOT, from Android's own set and from what Windows
// trusts, so a root an administrator, a company or a content filter installed
// in Windows is trusted by Android apps too.
//
// Win32 side: Windows' headers and BoringSSL's, no file descriptors.

#include "trust_store.h"

#include <windows.h>
#include <wincrypt.h>
// <wincrypt.h> names some of its constants like OpenSSL's types.
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

namespace gesher::net {
namespace {

using Der = std::vector<uint8_t>;
using Digest = std::array<uint8_t, SHA256_DIGEST_LENGTH>;

bool Trace() {
  const char* v = getenv("GESHER_TRACE_CACERTS");
  return v != nullptr && *v != '\0' && strcmp(v, "0") != 0;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// A Linux path of Gesher's path model (ADR 0010: drive-less paths are on the
// current drive) as a Win32 path.
std::wstring WinPath(const std::string& path) {
  std::wstring w = Widen(path);
  for (wchar_t& c : w) {
    if (c == L'/') c = L'\\';
  }
  return w;
}

Digest Sha256(const Der& der) {
  Digest d;
  SHA256(der.data(), der.size(), d.data());
  return d;
}

// Whether Windows lets `cert` vouch for TLS servers: an administrator can
// restrict a root's purposes (its enhanced key usage property).
bool ServerAuthAllowed(PCCERT_CONTEXT cert) {
  DWORD size = 0;
  if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size)) {
    return true;  // no property: whatever the certificate itself allows
  }
  std::vector<BYTE> buffer(size);
  auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
  if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, usage, &size)) {
    return true;
  }
  if (usage->cUsageIdentifier == 0) {
    // Good for every purpose, or (no error) for none.
    return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
  }
  for (DWORD i = 0; i < usage->cUsageIdentifier; i++) {
    if (strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0) return true;
  }
  return false;
}

enum class Take { kAll, kSelfSigned, kForDisallowed };

// The certificates of system store `name` at `location` (CERT_SYSTEM_STORE_*).
void ReadStore(DWORD location, const wchar_t* name, Take take, std::vector<Der>* out) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   location | CERT_STORE_OPEN_EXISTING_FLAG |
                                       CERT_STORE_READONLY_FLAG,
                                   name);
  if (store == nullptr) return;
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
    if ((cert->dwCertEncodingType & X509_ASN_ENCODING) == 0) continue;
    if (take == Take::kSelfSigned &&
        !CertCompareCertificateName(X509_ASN_ENCODING, &cert->pCertInfo->Subject,
                                    &cert->pCertInfo->Issuer)) {
      continue;
    }
    if (take != Take::kForDisallowed && !ServerAuthAllowed(cert)) continue;
    out->emplace_back(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
  }
  CertCloseStore(store, 0);
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out->clear();
  char buffer[16384];
  DWORD n = 0;
  bool ok = true;
  while ((ok = ::ReadFile(h, buffer, sizeof(buffer), &n, nullptr)) && n > 0) out->append(buffer, n);
  CloseHandle(h);
  return ok;
}

// Writes `path` whole: a temporary file next to it, then renamed over it.
bool WriteFileAtomically(const std::wstring& dir, const std::wstring& name,
                         const std::string& data) {
  std::wstring path = dir + L"\\" + name;
  wchar_t suffix[64];
  swprintf(suffix, 64, L".tmp-%lu-%lu", GetCurrentProcessId(), GetCurrentThreadId());
  std::wstring tmp = dir + L"\\." + name + suffix;
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size();
  CloseHandle(h);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

bool MakeDirectories(const std::wstring& dir) {
  if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
    return true;
  }
  if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
  size_t slash = dir.find_last_of(L'\\');
  if (slash == std::wstring::npos || slash == 0) return false;
  if (!MakeDirectories(dir.substr(0, slash))) return false;
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Android's own CA certificates, shipped next to the executable (cacerts\).
void ReadBundled(std::vector<Der>* out) {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::wstring dir(exe, n);
  dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\cacerts";
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string pem;
    if (!ReadWholeFile(dir + L"\\" + entry.cFileName, &pem)) continue;
    BIO* bio = BIO_new_mem_buf(pem.data(), pem.size());
    X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (x == nullptr) continue;
    uint8_t* der = nullptr;
    int len = i2d_X509(x, &der);
    if (len > 0) out->emplace_back(der, der + len);
    OPENSSL_free(der);
    X509_free(x);
  } while (FindNextFileW(find, &entry));
  FindClose(find);
}

struct Anchor {
  unsigned long hash;
  std::string pem;
};

bool IsCertFileName(const std::wstring& name) {
  // <8 hex digits>.<digits>, as Android names them.
  if (name.size() < 10 || name[8] != L'.') return false;
  for (size_t i = 0; i < 8; i++) {
    if (!iswxdigit(name[i])) return false;
  }
  for (size_t i = 9; i < name.size(); i++) {
    if (!iswdigit(name[i])) return false;
  }
  return true;
}

void Sync() {
  const char* android_root = getenv("ANDROID_ROOT");
  if (android_root == nullptr || *android_root == '\0') return;
  std::wstring dir = WinPath(std::string(android_root) + "/etc/security/cacerts");

  std::vector<Der> bundled, windows, disallowed;
  ReadBundled(&bundled);
  const char* use_windows = getenv("GESHER_WINDOWS_CERTS");
  bool from_windows = use_windows == nullptr || strcmp(use_windows, "0") != 0;
  for (DWORD location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
    if (from_windows) {
      ReadStore(location, L"ROOT", Take::kAll, &windows);
      ReadStore(location, L"AuthRoot", Take::kAll, &windows);
      // Roots installed among the intermediate CAs.
      ReadStore(location, L"CA", Take::kSelfSigned, &windows);
    }
    ReadStore(location, L"Disallowed", Take::kForDisallowed, &disallowed);
  }
  std::set<Digest> distrusted;
  for (const Der& der : disallowed) distrusted.insert(Sha256(der));

  // By digest: each certificate once, in a stable order.
  std::map<Digest, Anchor> anchors;
  size_t from_android = 0, from_store = 0, skipped = 0;
  auto add = [&](const Der& der, size_t* count) {
    Digest digest = Sha256(der);
    if (anchors.count(digest) != 0) return;
    if (distrusted.count(digest) != 0) {
      skipped++;
      return;
    }
    const uint8_t* p = der.data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (x == nullptr) return;
    Anchor anchor;
    anchor.hash = X509_NAME_hash_old(X509_get_subject_name(x));
    BIO* bio = BIO_new(BIO_s_mem());
    if (PEM_write_bio_X509(bio, x)) {
      const uint8_t* data = nullptr;
      size_t len = 0;
      BIO_mem_contents(bio, &data, &len);
      anchor.pem.assign(reinterpret_cast<const char*>(data), len);
    }
    BIO_free(bio);
    X509_free(x);
    if (anchor.pem.empty()) return;
    anchors.emplace(digest, std::move(anchor));
    (*count)++;
  };
  for (const Der& der : bundled) add(der, &from_android);
  for (const Der& der : windows) add(der, &from_store);

  // Names: <hash>.<n>, n counting certificates whose subjects hash alike.
  std::map<std::wstring, const Anchor*> files;
  std::map<unsigned long, int> next;
  std::string manifest;
  for (const auto& [digest, anchor] : anchors) {
    wchar_t name[32];
    swprintf(name, 32, L"%08lx.%d", anchor.hash, next[anchor.hash]++);
    files[name] = &anchor;
    char line[128];
    snprintf(line, sizeof(line), "%08lx.%d ", anchor.hash, next[anchor.hash] - 1);
    manifest += line;
    for (uint8_t b : digest) {
      snprintf(line, sizeof(line), "%02x", b);
      manifest += line;
    }
    manifest += '\n';
  }

  // One process at a time rewrites the directory.
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\gesher-system-cacerts");
  if (mutex != nullptr) WaitForSingleObject(mutex, 30000);
  std::string current;
  bool changed = !ReadWholeFile(dir + L"\\.gesher-cacerts", &current) || current != manifest;
  size_t written = 0, removed = 0;
  bool ok = true;
  if (changed) {
    ok = MakeDirectories(dir);
    for (const auto& [name, anchor] : files) {
      if (!ok) break;
      std::string existing;
      if (ReadWholeFile(dir + L"\\" + name, &existing) && existing == anchor->pem) continue;
      if (WriteFileAtomically(dir, name, anchor->pem)) {
        written++;
      } else {
        ok = false;
      }
    }
    WIN32_FIND_DATAW entry;
    HANDLE find = ok ? FindFirstFileW((dir + L"\\*").c_str(), &entry) : INVALID_HANDLE_VALUE;
    if (find != INVALID_HANDLE_VALUE) {
      do {
        std::wstring name = entry.cFileName;
        if (IsCertFileName(name) && files.count(name) == 0 &&
            DeleteFileW((dir + L"\\" + name).c_str())) {
          removed++;
        }
      } while (FindNextFileW(find, &entry));
      FindClose(find);
    }
    if (ok) ok = WriteFileAtomically(dir, L".gesher-cacerts", manifest);
  }
  if (mutex != nullptr) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  if (!ok) {
    fprintf(stderr, "gesher: could not write the system CA certificates to %s/etc/security/cacerts "
            "(error %lu)\n", android_root, GetLastError());
  } else if (Trace()) {
    fprintf(stderr,
            "gesher: system CA certificates: %zu (%zu Android's, %zu more from Windows' stores, "
            "%zu distrusted by Windows left out); %s (%zu written, %zu removed)\n",
            anchors.size(), from_android, from_store, skipped,
            changed ? "updated" : "unchanged", written, removed);
  }
}

}  // namespace

void SyncSystemCaCerts() { Sync(); }

}  // namespace gesher::net
