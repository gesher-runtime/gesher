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
// A root installed on a phone by its user or a device admin is a user-added CA
// instead: KeyChain writes it to /data/misc/user/<id>/cacerts-added, and
// TrustedCertificateStore (the AndroidCAStore KeyStore) lists it as "user:".
// Apps that trust only their own roots plus the user's (Chrome, apps whose
// network security config has <certificates src="user"/>, and those targeting
// API 23 or lower) look there. The roots that were installed on this PC rather
// than distributed by Microsoft are also written there (LocallyInstalledRoots).
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
#include <ctype.h>
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

using trust_store_internal::Der;
using trust_store_internal::Sha1;
using Digest = std::array<uint8_t, SHA256_DIGEST_LENGTH>;

// Gesher's device has one user, user 0: UserHandle.myUserId() of every app
// process, whose Environment.getUserConfigDirectory is $ANDROID_DATA/misc/user/0.
constexpr int kUserId = 0;

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

Sha1 Sha1Of(const Der& der) {
  Sha1 d;
  SHA1(der.data(), der.size(), d.data());
  return d;
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

#ifndef CERT_AUTH_ROOT_SHA256_HASH_PROP_ID
#define CERT_AUTH_ROOT_SHA256_HASH_PROP_ID 98
#endif

// The roots of the ROOT store at `location` that Windows' automatic root update
// put there from Microsoft's list: it gives them the list entry's SHA-256 as a
// property, which a root installed otherwise does not have.
void ReadRootProgramMarks(DWORD location, std::set<Sha1>* out) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   location | CERT_STORE_OPEN_EXISTING_FLAG |
                                       CERT_STORE_READONLY_FLAG,
                                   L"ROOT");
  if (store == nullptr) return;
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(cert, CERT_AUTH_ROOT_SHA256_HASH_PROP_ID, nullptr,
                                           &size)) {
      continue;
    }
    Sha1 thumbprint;
    SHA1(cert->pbCertEncoded, cert->cbCertEncoded, thumbprint.data());
    out->insert(thumbprint);
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

// The named stream that marks a user CA file as Gesher's: the file's name and
// the SHA-256 of the data Gesher wrote, in hex. Android reads only the file's
// data.
const wchar_t kOwnerStream[] = L"gesher-owner";

// Writes dir\name whole: a temporary file in `staging` (on the same volume),
// then renamed over it. Staging outside cacerts keeps half-written files out
// of the directory Conscrypt reads.
bool WriteFileAtomically(const std::wstring& dir, const std::wstring& name,
                         const std::string& data, const std::wstring& staging,
                         const std::string* owner_mark = nullptr, bool replace = true) {
  std::wstring path = dir + L"\\" + name;
  wchar_t suffix[64];
  swprintf(suffix, 64, L".tmp-%lu-%lu", GetCurrentProcessId(), GetCurrentThreadId());
  std::wstring tmp = staging + L"\\." + name + suffix;
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size();
  CloseHandle(h);
  if (ok && owner_mark != nullptr) {
    // On the temporary file, so the rename moves data and mark at once. A
    // volume without named streams (FAT) keeps no mark; the caller's manifest
    // still records the file.
    HANDLE mark = CreateFileW((tmp + L":" + kOwnerStream).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (mark != INVALID_HANDLE_VALUE) {
      DWORD n = 0;
      ::WriteFile(mark, owner_mark->data(), static_cast<DWORD>(owner_mark->size()), &n, nullptr);
      CloseHandle(mark);
    }
  }
  // Without `replace`, a file already at the name stays and the rename fails
  // with ERROR_ALREADY_EXISTS, which the caller sees.
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
  if (!ok) {
    DWORD error = GetLastError();
    DeleteFileW(tmp.c_str());
    SetLastError(error);
  }
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

bool IsStagedTrustFile(const std::wstring& name) {
  const size_t marker = name.rfind(L".tmp-");
  if (name.empty() || name.front() != L'.' || marker == std::wstring::npos || marker <= 1)
    return false;
  const std::wstring target = name.substr(1, marker - 1);
  if (target != L".gesher-cacerts" && target != L".gesher-user-cacerts" && !IsCertFileName(target))
    return false;
  const size_t pid = marker + 5;
  const size_t separator = name.find(L'-', pid);
  if (separator == std::wstring::npos || separator == pid || separator + 1 >= name.size()) return false;
  for (size_t i = pid; i < name.size(); i++) {
    if (i != separator && (name[i] < L'0' || name[i] > L'9')) return false;
  }
  return true;
}

// Removes the .<name>.tmp-<pid>-<tid> files WriteFileAtomically stages in `dir`
// (earlier versions staged them in cacerts itself). Called with the writers'
// mutex held, so none of them belongs to a live write.
void RemoveStaleTemporaryFiles(const std::wstring& dir) {
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((dir + L"\\.*.tmp-*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        IsStagedTrustFile(entry.cFileName)) {
      DeleteFileW((dir + L"\\" + entry.cFileName).c_str());
    }
  } while (FindNextFileW(find, &entry));
  FindClose(find);
}

void Sync() {
  const char* android_root = getenv("ANDROID_ROOT");
  if (android_root == nullptr || *android_root == '\0') return;
  // The manifest and temporary files live in $ANDROID_ROOT/etc/security, so
  // cacerts holds only certificates, as on Android: Conscrypt's
  // DirectoryCertificateSource reads every file in it as one.
  std::wstring security = WinPath(std::string(android_root) + "/etc/security");
  std::wstring dir = security + L"\\cacerts";
  std::wstring manifest_path = security + L"\\.gesher-cacerts";

  std::vector<Der> bundled, roots, windows, disallowed;
  ReadBundled(&bundled);
  const char* use_windows = getenv("GESHER_WINDOWS_CERTS");
  bool from_windows = use_windows == nullptr || strcmp(use_windows, "0") != 0;
  for (DWORD location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
    if (from_windows) {
      // The ROOT stores: where a root the user, an administrator, group policy
      // or an enterprise installs is, besides Microsoft's own.
      ReadStore(location, L"ROOT", Take::kAll, &roots);
      ReadStore(location, L"AuthRoot", Take::kAll, &windows);
      // Roots installed among the intermediate CAs.
      ReadStore(location, L"CA", Take::kSelfSigned, &windows);
    }
    ReadStore(location, L"Disallowed", Take::kForDisallowed, &disallowed);
  }
  windows.insert(windows.begin(), roots.begin(), roots.end());
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

  // The user-added CAs: the ROOT stores' roots installed on this PC. Without
  // Microsoft's list no root can be told apart, and none is added.
  const char* android_data = getenv("ANDROID_DATA");
  bool user_store = android_data != nullptr && *android_data != '\0';
  std::set<Sha1> microsoft;
  bool classified = user_store && !roots.empty() &&
                    trust_store_internal::ReadMicrosoftRootProgram(&microsoft);
  std::vector<Der> local;
  if (classified) {
    // Also what automatic root update added from a list that is not either of
    // these any more.
    for (DWORD location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
      ReadRootProgramMarks(location, &microsoft);
    }
    local = trust_store_internal::LocallyInstalledRoots(roots, microsoft, bundled, disallowed);
  }

  // One process at a time rewrites the directories.
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\gesher-system-cacerts");
  DWORD waited = mutex != nullptr ? WaitForSingleObject(mutex, 30000) : WAIT_FAILED;
  if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
    DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    if (mutex != nullptr) CloseHandle(mutex);
    fprintf(stderr, "gesher: could not lock the system CA certificate store (error %lu)\n", error);
    return;
  }
  std::string current;
  // Earlier versions kept the manifest in cacerts itself.
  DeleteFileW((dir + L"\\.gesher-cacerts").c_str());
  // Temporary files of a writer that died before its rename (every writer
  // holds the mutex while its temporary files exist).
  RemoveStaleTemporaryFiles(security);
  RemoveStaleTemporaryFiles(dir);
  // The manifest is outside the directory: it describes the directory only
  // while the directory and every file it lists exist.
  DWORD dir_attributes = GetFileAttributesW(dir.c_str());
  bool changed = dir_attributes == INVALID_FILE_ATTRIBUTES ||
                 (dir_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                 !ReadWholeFile(manifest_path, &current) || current != manifest;
  for (auto it = files.begin(); !changed && it != files.end(); ++it) {
    DWORD attributes = GetFileAttributesW((dir + L"\\" + it->first).c_str());
    changed = attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }
  size_t written = 0, removed = 0;
  bool ok = true;
  if (changed) {
    ok = MakeDirectories(dir);
    for (const auto& [name, anchor] : files) {
      if (!ok) break;
      std::string existing;
      if (ReadWholeFile(dir + L"\\" + name, &existing) && existing == anchor->pem) continue;
      if (WriteFileAtomically(dir, name, anchor->pem, security)) {
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
    if (ok) ok = WriteFileAtomically(security, L".gesher-cacerts", manifest, security);
  }
  DWORD error = ok ? 0 : GetLastError();
  trust_store_internal::UserSyncResult user;
  std::string user_dir;
  if (user_store) {
    char sub[32];
    snprintf(sub, sizeof(sub), "/misc/user/%d", kUserId);
    user_dir = std::string(android_data) + sub;
    user = trust_store_internal::SyncUserCaDirectory(WinPath(user_dir), local);
  }
  DWORD user_error = user.ok ? 0 : user.error;
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  if (!ok) {
    fprintf(stderr, "gesher: could not write the system CA certificates to %s/etc/security/cacerts "
            "(error %lu)\n", android_root, error);
  } else if (Trace()) {
    fprintf(stderr,
            "gesher: system CA certificates: %zu (%zu Android's, %zu more from Windows' stores, "
            "%zu distrusted by Windows left out); %s (%zu written, %zu removed)\n",
            anchors.size(), from_android, from_store, skipped,
            changed ? "updated" : "unchanged", written, removed);
  }
  if (user_store && !user.ok) {
    fprintf(stderr, "gesher: could not write the user CA certificates to %s/cacerts-added "
            "(error %lu)\n", user_dir.c_str(), user_error);
  } else if (user_store && Trace()) {
    fprintf(stderr,
            "gesher: user CA certificates (roots installed on this PC): %zu%s (%zu written, "
            "%zu removed)\n",
            local.size(),
            classified || roots.empty() ? "" : ", none: Microsoft's root program list is unreadable",
            user.written, user.removed);
    for (const Der& der : local) {
      const uint8_t* p = der.data();
      X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
      if (x == nullptr) continue;
      char subject[512];
      X509_NAME_oneline(X509_get_subject_name(x), subject, sizeof(subject));
      std::string hex;
      for (uint8_t b : Sha1Of(der)) {
        char two[3];
        snprintf(two, sizeof(two), "%02X", b);
        hex += two;
      }
      fprintf(stderr, "gesher:   user CA %s %s\n", hex.c_str(), subject);
      X509_free(x);
    }
  }
}

}  // namespace

void SyncSystemCaCerts() { Sync(); }

namespace trust_store_internal {
namespace {

// One AuthRoot certificate trust list: signed by Microsoft for its root
// program (subject usage szOID_ROOT_LIST_SIGNER), its subjects named by the
// SHA-1 of their DER encoding.
bool AddRootProgramList(const BYTE* data, DWORD size, std::set<Sha1>* out) {
  if (data == nullptr || size == 0) return false;
  PCCTL_CONTEXT ctl = CertCreateCTLContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data, size);
  if (ctl == nullptr) return false;
  const CTL_INFO* info = ctl->pCtlInfo;
  bool root_list = false;
  for (DWORD i = 0; i < info->SubjectUsage.cUsageIdentifier; i++) {
    if (strcmp(info->SubjectUsage.rgpszUsageIdentifier[i], szOID_ROOT_LIST_SIGNER) == 0) {
      root_list = true;
    }
  }
  bool by_sha1 = info->SubjectAlgorithm.pszObjId != nullptr &&
                 strcmp(info->SubjectAlgorithm.pszObjId, szOID_OIWSEC_sha1) == 0;
  size_t added = 0;
  if (root_list && by_sha1) {
    for (DWORD i = 0; i < info->cCTLEntry; i++) {
      const CRYPT_DATA_BLOB& id = info->rgCTLEntry[i].SubjectIdentifier;
      if (id.cbData != 20) continue;
      Sha1 thumbprint;
      memcpy(thumbprint.data(), id.pbData, 20);
      out->insert(thumbprint);
      added++;
    }
  }
  CertFreeCTLContext(ctl);
  return added > 0;
}

BOOL CALLBACK AddResourceList(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  auto* found = reinterpret_cast<std::pair<std::set<Sha1>*, bool>*>(param);
  HRSRC resource = FindResourceW(module, name, type);
  HGLOBAL loaded = resource != nullptr ? LoadResource(module, resource) : nullptr;
  const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
  if (data != nullptr &&
      AddRootProgramList(static_cast<const BYTE*>(data), SizeofResource(module, resource),
                         found->first)) {
    found->second = true;
  }
  return TRUE;
}

}  // namespace

bool ReadMicrosoftRootProgram(std::set<Sha1>* thumbprints) {
  bool found = false;
  // The list Windows' automatic root update last fetched (absent when it is
  // turned off). Only administrators can change it.
  DWORD size = 0;
  const wchar_t* key = L"SOFTWARE\\Microsoft\\SystemCertificates\\AuthRoot\\AutoUpdate";
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EncodedCtl", RRF_RT_REG_BINARY, nullptr, nullptr,
                   &size) == ERROR_SUCCESS && size > 0) {
    std::vector<BYTE> data(size);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EncodedCtl", RRF_RT_REG_BINARY, nullptr,
                     data.data(), &size) == ERROR_SUCCESS) {
      found |= AddRootProgramList(data.data(), size, thumbprints);
    }
  }
  // The list crypt32.dll was built with (its AUTHROOTSTL resource), which
  // Windows uses when it has fetched none.
  HMODULE crypt32 = GetModuleHandleW(L"crypt32.dll");
  if (crypt32 != nullptr) {
    std::pair<std::set<Sha1>*, bool> state{thumbprints, false};
    EnumResourceNamesW(crypt32, L"AUTHROOTSTL", AddResourceList,
                       reinterpret_cast<LONG_PTR>(&state));
    found |= state.second;
  }
  return found;
}

std::vector<Der> LocallyInstalledRoots(const std::vector<Der>& roots,
                                       const std::set<Sha1>& microsoft,
                                       const std::vector<Der>& android,
                                       const std::vector<Der>& disallowed) {
  std::set<Digest> excluded;
  for (const Der& der : android) excluded.insert(Sha256(der));
  for (const Der& der : disallowed) excluded.insert(Sha256(der));
  std::map<Digest, const Der*> local;
  for (const Der& der : roots) {
    Digest digest = Sha256(der);
    if (excluded.count(digest) != 0 || microsoft.count(Sha1Of(der)) != 0) continue;
    const uint8_t* p = der.data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (x == nullptr) continue;
    X509_free(x);
    local.emplace(digest, &der);
  }
  std::vector<Der> out;
  for (const auto& [digest, der] : local) out.push_back(*der);
  return out;
}

namespace {

std::wstring UserCertName(unsigned long hash, int index) {
  wchar_t name[32];
  swprintf(name, 32, L"%08lx.%d", hash, index);
  return name;
}

std::string Hex(const Digest& digest) {
  std::string hex;
  for (uint8_t b : digest) {
    char two[3];
    snprintf(two, sizeof(two), "%02x", b);
    hex += two;
  }
  return hex;
}

bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsTombstone(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
         (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && data.nFileSizeHigh == 0 &&
         data.nFileSizeLow == 0;
}

// TrustedCertificateStore.deleteCertificateEntry for a user alias: the file is
// truncated to a tombstone, so later indexes stay reachable, and then the
// tombstones that end the hash's run are deleted (removeUnnecessaryTombstones).
bool RemoveUserEntry(const std::wstring& dir, const std::wstring& name) {
  std::wstring path = dir + L"\\" + name;
  // Shared like Gesher's own file layer opens files, so a reader (another
  // process, a scanner) does not keep the entry from being removed.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         TRUNCATE_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
  CloseHandle(h);
  unsigned long hash = wcstoul(name.substr(0, 8).c_str(), nullptr, 16);
  int index = static_cast<int>(wcstol(name.c_str() + 9, nullptr, 10));
  if (Exists(dir + L"\\" + UserCertName(hash, index + 1))) return true;
  for (; index >= 0; index--) {
    std::wstring tombstone = dir + L"\\" + UserCertName(hash, index);
    if (!IsTombstone(tombstone)) break;
    if (!DeleteFileW(tombstone.c_str())) return false;
  }
  return true;
}

}  // namespace

namespace {

std::string TrimLower(const std::string& text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r\n");
  std::string out = text.substr(begin, end - begin + 1);
  for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return out;
}

// The digest of dir\name if it is a non-empty file that holds what Gesher
// recorded for it (`recorded`: a hex SHA-256).
bool Holds(const std::wstring& path, const std::string& recorded, Digest* digest) {
  std::string contents;
  if (!ReadWholeFile(path, &contents) || contents.empty()) return false;
  *digest = Sha256(Der(contents.begin(), contents.end()));
  return Hex(*digest) == TrimLower(recorded);
}

std::string ManifestText(const std::map<std::wstring, Digest>& entries) {
  std::string listed;
  for (const auto& [name, digest] : entries) {
    listed += std::string(name.begin(), name.end()) + " " + Hex(digest) + "\n";
  }
  return listed;
}

}  // namespace

void (*g_before_user_entry_write)(const std::wstring& path) = nullptr;

UserSyncResult SyncUserCaDirectory(const std::wstring& user_dir,
                                   const std::vector<Der>& certificates) {
  UserSyncResult result;
  // Records a failure with the error of the call that failed, taken there:
  // later calls (cleanup, the next entry) overwrite GetLastError.
  bool ok = true;
  auto fail = [&](DWORD error) {
    if (result.error == 0) result.error = error;
    ok = false;
  };
  const std::wstring dir = user_dir + L"\\cacerts-added";
  const std::wstring manifest_path = user_dir + L"\\.gesher-user-cacerts";
  // Temporary files of a writer that died before its rename.
  RemoveStaleTemporaryFiles(user_dir);

  struct Wanted {
    unsigned long hash;
    const Der* der;
  };
  std::map<Digest, Wanted> wanted;
  for (const Der& der : certificates) {
    const uint8_t* p = der.data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (x == nullptr) continue;
    wanted.emplace(Sha256(der), Wanted{X509_NAME_hash_old(X509_get_subject_name(x)), &der});
    X509_free(x);
  }

  // Gesher's entries: those the manifest names, and those whose owner mark
  // (written with the file) names, that still hold what was written. Either
  // record alone is enough, so a lost or unwritable manifest does not leave
  // an entry behind. Anything else in the directory is not Gesher's.
  std::map<std::wstring, Digest> owned;
  // Gesher's entries (by a manifest line or the owner mark) that exist but
  // cannot be read now (opened without sharing): listed in the manifest as
  // recorded, so the entry stays recorded until a later sync can check and,
  // if need be, remove it.
  std::map<std::wstring, std::string> unreadable;
  std::string manifest;
  bool had_manifest = ReadWholeFile(manifest_path, &manifest);
  for (size_t at = 0; had_manifest && at < manifest.size();) {
    size_t end = manifest.find('\n', at);
    if (end == std::string::npos) end = manifest.size();
    std::string line = TrimLower(manifest.substr(at, end - at));
    at = end + 1;
    size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) continue;
    std::wstring name(line.begin(), line.begin() + space);
    if (!IsCertFileName(name)) continue;
    Digest digest;
    if (Holds(dir + L"\\" + name, line.substr(space + 1), &digest)) {
      owned[name] = digest;
      continue;
    }
    std::string ignored;
    DWORD attributes = GetFileAttributesW((dir + L"\\" + name).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        !ReadWholeFile(dir + L"\\" + name, &ignored)) {
      fail(GetLastError());
      unreadable[name] = TrimLower(line.substr(space + 1));
    }
  }
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      std::wstring name = entry.cFileName;
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !IsCertFileName(name) ||
          owned.count(name) != 0 || unreadable.count(name) != 0) {
        continue;
      }
      std::wstring path = dir + L"\\" + name;
      // "<name> <sha-256>": a copy carrying the stream to another name (as
      // Explorer and copy do) does not match it.
      std::string mark;
      Digest digest;
      if (!ReadWholeFile(path + L":" + kOwnerStream, &mark)) continue;
      mark = TrimLower(mark);
      std::string lower_name = TrimLower(std::string(name.begin(), name.end()));
      size_t space = mark.find(' ');
      if (space == std::string::npos || mark.substr(0, space) != lower_name) continue;
      if (Holds(path, mark.substr(space + 1), &digest)) {
        owned[name] = digest;
        continue;
      }
      // The stream stays readable while the file itself is held without
      // sharing: the mark alone then records it (the manifest may be lost),
      // as a manifest line does for an entry that cannot be read.
      std::string ignored;
      if (!ReadWholeFile(path, &ignored)) {
        fail(GetLastError());
        unreadable[name] = mark.substr(space + 1);
      }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
  }

  // Gesher's entries that are no longer wanted: the root left Windows' store.
  for (auto it = owned.begin(); it != owned.end();) {
    if (wanted.count(it->second) != 0) {
      ++it;
      continue;
    }
    if (RemoveUserEntry(dir, it->first)) {
      result.removed++;
      it = owned.erase(it);
    } else {
      fail(GetLastError());
      ++it;
    }
  }

  // Wanted ones not in the directory: after the last index of their hash, as
  // TrustedCertificateStore.installCertificate adds one. A tombstone keeps its
  // index. Like its findCert, the run ends at the first index that is not a
  // file: an entry there would be unreachable, so a directory in the way is
  // a failure, not a reason to go past it. An entry that could not be
  // removed stays where it is and does not hold up the others. Gesher's own
  // entry for the certificate that cannot be read now (its manifest line
  // or owner mark gives the digest) is present: writing it again would make a second alias.
  enum class Slot { kFree, kPresent, kBlocked };
  std::set<std::wstring> planned;
  auto find_slot = [&](unsigned long hash, const Digest& digest, const std::string& der,
                       std::wstring* name) {
    const std::string hex = Hex(digest);
    for (int index = 0;; index++) {
      *name = UserCertName(hash, index);
      if (planned.count(*name) != 0) continue;
      auto recorded = unreadable.find(*name);
      if (recorded != unreadable.end() && recorded->second == hex) return Slot::kPresent;
      std::wstring path = dir + L"\\" + *name;
      DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) return Slot::kFree;
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return Slot::kBlocked;
      std::string contents;
      if (ReadWholeFile(path, &contents) && contents == der) return Slot::kPresent;
    }
  };
  struct Pending {
    std::wstring name;
    Digest digest;
    unsigned long hash;
    const Der* der;
  };
  std::vector<Pending> pending;
  for (const auto& [digest, entry] : wanted) {
    std::string der(entry.der->begin(), entry.der->end());
    std::wstring name;
    Slot slot = find_slot(entry.hash, digest, der, &name);
    if (slot == Slot::kPresent) continue;
    if (slot == Slot::kBlocked) {
      fail(ERROR_CANNOT_MAKE);  // a directory where the entry would go
      continue;
    }
    planned.insert(name);
    pending.push_back(Pending{name, digest, entry.hash, entry.der});
  }

  // The manifest names the entries before they are written, so one written
  // just before a crash is still Gesher's; it is rewritten only when it
  // changes.
  auto write_manifest = [&](const std::map<std::wstring, Digest>& entries) {
    std::string listed = ManifestText(entries);
    for (const auto& [name, hex] : unreadable) {
      if (entries.count(name) == 0) listed += std::string(name.begin(), name.end()) + " " + hex + "\n";
    }
    if (listed.empty()) {
      if (!had_manifest || DeleteFileW(manifest_path.c_str())) return true;
      DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) return true;
      fail(error);
      return false;
    }
    if (had_manifest && listed == manifest) return true;
    if (!MakeDirectories(user_dir) ||
        !WriteFileAtomically(user_dir, L".gesher-user-cacerts", listed, user_dir)) {
      fail(GetLastError());
      return false;
    }
    had_manifest = true;
    manifest = listed;
    return true;
  };
  std::map<std::wstring, Digest> intended = owned;
  for (const Pending& p : pending) intended[p.name] = p.digest;
  if (!write_manifest(intended)) {
    // Without the manifest nothing is written: the owner mark alone would
    // be lost on a volume without named streams.
    result.ok = false;  // fail() recorded the error
    return result;
  }
  // A new entry never replaces a file: one that appeared at its name since
  // the scan is someone else's. The entry then takes the next free index,
  // named in the manifest first like any other.
  for (const Pending& p : pending) {
    std::string der(p.der->begin(), p.der->end());
    std::wstring name = p.name;
    bool done = false;
    for (int attempt = 0; attempt < 8 && !done; attempt++) {
      std::string text(name.begin(), name.end());
      std::string mark = text + " " + Hex(p.digest);
      if (g_before_user_entry_write != nullptr) g_before_user_entry_write(dir + L"\\" + name);
      if (MakeDirectories(dir) && WriteFileAtomically(dir, name, der, user_dir, &mark, false)) {
        owned[name] = p.digest;
        result.written++;
        done = true;
        break;
      }
      DWORD error = GetLastError();
      if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
        fail(error);
        break;
      }
      intended.erase(name);
      planned.erase(name);
      Slot slot = find_slot(p.hash, p.digest, der, &name);
      if (slot == Slot::kPresent) {
        done = true;  // Someone else's copy: not Gesher's, nothing to write.
        break;
      }
      if (slot == Slot::kBlocked) {
        fail(ERROR_CANNOT_MAKE);
        break;
      }
      planned.insert(name);
      intended[name] = p.digest;
      if (!write_manifest(intended)) break;
    }
    if (!done) fail(ERROR_ALREADY_EXISTS);  // only if nothing above recorded one
  }
  write_manifest(owned);
  result.ok = ok;
  return result;
}

}  // namespace trust_store_internal

}  // namespace gesher::net
