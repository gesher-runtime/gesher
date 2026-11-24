// The user-added CA certificates Gesher writes (compat/net/trust_store.cc,
// docs/network.md): which of Windows' roots count as installed on this PC, and
// $ANDROID_DATA/misc/user/0/cacerts-added laid out as Android's
// TrustedCertificateStore keeps it (DER <hash>.<n>, tombstones, appends), with
// only Gesher's own entries ever changed.
//
// The certificates are synthetic, made here, and every directory is a
// temporary one: the classification and the directory writer are called
// directly, never through Windows' stores. The last part runs the production
// sync against this machine's real stores, in temporary roots, and checks that
// each root it lists as user-added is in Windows' ROOT store and not in
// Microsoft's root program.

#include <windows.h>
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <openssl/bn.h>
#include <openssl/mem.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include "trust_store.h"

using gesher::net::trust_store_internal::Der;
using gesher::net::trust_store_internal::Sha1;
namespace internal = gesher::net::trust_store_internal;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(cond)) {                                                                       \
      fprintf(stderr, "FAIL: %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                                        \
    }                                                                                    \
  } while (0)

// A self-signed CA certificate named `cn` with a fresh Ed25519 key: two made
// with one name have the same subject hash and different contents.
static Der MakeRoot(const char* cn) {
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  EVP_PKEY* key = nullptr;
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);
  X509* x = X509_new();
  X509_set_version(x, X509_VERSION_3);
  static long serial = 1;
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial++);
  X509_gmtime_adj(X509_getm_notBefore(x), -86400);
  X509_gmtime_adj(X509_getm_notAfter(x), 86400L * 30);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("Gesher trust store test"), -1,
                             -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
  X509_set_issuer_name(x, name);
  X509_set_pubkey(x, key);
  X509_sign(x, key, nullptr);
  uint8_t* der = nullptr;
  int len = i2d_X509(x, &der);
  Der out;
  if (len > 0) out.assign(der, der + len);
  OPENSSL_free(der);
  X509_free(x);
  EVP_PKEY_free(key);
  return out;
}

static Sha1 Thumbprint(const Der& der) {
  Sha1 d;
  SHA1(der.data(), der.size(), d.data());
  return d;
}

static std::wstring HashName(const Der& der, int index) {
  const uint8_t* p = der.data();
  X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
  unsigned long hash = X509_NAME_hash_old(X509_get_subject_name(x));
  X509_free(x);
  wchar_t name[32];
  swprintf(name, 32, L"%08lx.%d", hash, index);
  return name;
}

static bool WriteBytes(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size();
  CloseHandle(h);
  return ok;
}

// The file's contents, or "<missing>".
static std::string Contents(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return "<missing>";
  std::string out;
  char buffer[16384];
  DWORD n = 0;
  while (::ReadFile(h, buffer, sizeof(buffer), &n, nullptr) && n > 0) out.append(buffer, n);
  CloseHandle(h);
  return out;
}

static std::string AsString(const Der& der) { return std::string(der.begin(), der.end()); }

static std::set<std::wstring> List(const std::wstring& dir) {
  std::set<std::wstring> names;
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return names;
  do {
    std::wstring name = entry.cFileName;
    if (name != L"." && name != L"..") names.insert(name);
  } while (FindNextFileW(find, &entry));
  FindClose(find);
  return names;
}

static void RemoveTree(const std::wstring& dir) {
  for (const std::wstring& name : List(dir)) {
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveTree(path);
    } else {
      DeleteFileW(path.c_str());
    }
  }
  RemoveDirectoryW(dir.c_str());
}

static std::string Utf8(const std::wstring& w) {
  std::string s(w.size() * 3, '\0');
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(),
                                static_cast<int>(s.size()), nullptr, nullptr);
  s.resize(len > 0 ? len : 0);
  return s;
}

static void Classification() {
  Der local = MakeRoot("Locally installed root");
  Der microsoft = MakeRoot("Root program root");
  Der android = MakeRoot("Android CA set root");
  Der distrusted = MakeRoot("Distrusted root");
  std::set<Sha1> program = {Thumbprint(microsoft)};
  std::vector<Der> roots = {microsoft, local, android, distrusted, local};
  std::vector<Der> result =
      internal::LocallyInstalledRoots(roots, program, {android}, {distrusted});
  // Only the root that is in neither list, once.
  CHECK(result.size() == 1);
  CHECK(!result.empty() && result[0] == local);
  // No list: nothing is Microsoft's, so every root not otherwise excluded is local.
  CHECK(internal::LocallyInstalledRoots(roots, {}, {android}, {distrusted}).size() == 2);

  // This machine's lists: Windows has at least the one crypt32.dll was built
  // with, Microsoft's whole root program (hundreds of roots), and a root made
  // here is not in it.
  std::set<Sha1> real;
  CHECK(internal::ReadMicrosoftRootProgram(&real));
  CHECK(real.size() > 100);
  CHECK(real.count(Thumbprint(local)) == 0);
  printf("Microsoft's root program on this machine: %zu roots\n", real.size());
}

static void Directory(const std::wstring& root) {
  std::wstring user = root + L"\\misc\\user\\0";
  std::wstring added = user + L"\\cacerts-added";
  std::wstring manifest = user + L"\\.gesher-user-cacerts";

  // Nothing to add, nothing there: no directory is made.
  internal::UserSyncResult r = internal::SyncUserCaDirectory(user, {});
  CHECK(r.ok && r.written == 0 && r.removed == 0);
  CHECK(GetFileAttributesW(user.c_str()) == INVALID_FILE_ATTRIBUTES);

  // Two roots with one subject, so one hash: <hash>.0 and <hash>.1, DER.
  Der a = MakeRoot("Same subject");
  Der b = MakeRoot("Same subject");
  Der c = MakeRoot("Same subject");
  Der other = MakeRoot("Another subject");
  CHECK(HashName(a, 0) == HashName(b, 0) && HashName(a, 0) == HashName(c, 0));
  CHECK(HashName(a, 0) != HashName(other, 0));
  r = internal::SyncUserCaDirectory(user, {a, b});
  CHECK(r.ok && r.written == 2 && r.removed == 0);
  std::string at0 = Contents(added + L"\\" + HashName(a, 0));
  std::string at1 = Contents(added + L"\\" + HashName(a, 1));
  CHECK((at0 == AsString(a) && at1 == AsString(b)) || (at0 == AsString(b) && at1 == AsString(a)));
  CHECK(List(added).size() == 2);
  // The manifest is next to the directory, which holds only certificates.
  CHECK(Contents(manifest) != "<missing>");
  for (const std::wstring& name : List(user)) {
    CHECK(name == L"cacerts-added" || name == L".gesher-user-cacerts");
  }

  // Unchanged: nothing written.
  WIN32_FILE_ATTRIBUTE_DATA before = {}, after = {};
  GetFileAttributesExW(manifest.c_str(), GetFileExInfoStandard, &before);
  r = internal::SyncUserCaDirectory(user, {b, a});
  CHECK(r.ok && r.written == 0 && r.removed == 0);
  GetFileAttributesExW(manifest.c_str(), GetFileExInfoStandard, &after);
  CHECK(CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0);

  // A certificate Gesher did not write (installed through Android, say):
  // never changed or removed, and not written twice.
  std::wstring foreign = added + L"\\" + HashName(other, 0);
  CHECK(WriteBytes(foreign, AsString(other)));
  r = internal::SyncUserCaDirectory(user, {a, b, other});
  CHECK(r.ok && r.written == 0 && r.removed == 0);
  CHECK(Contents(foreign) == AsString(other));

  // The root at index 0 leaves Windows' store: a tombstone (an empty file)
  // keeps index 1 reachable, as TrustedCertificateStore.deleteCertificateEntry
  // leaves it.
  Der first = at0 == AsString(a) ? a : b;
  Der second = at0 == AsString(a) ? b : a;
  r = internal::SyncUserCaDirectory(user, {second});
  CHECK(r.ok && r.written == 0 && r.removed == 1);
  CHECK(Contents(added + L"\\" + HashName(a, 0)).empty());
  CHECK(Contents(added + L"\\" + HashName(a, 1)) == AsString(second));
  CHECK(Contents(foreign) == AsString(other));

  // A new root with that subject goes after the last index, as
  // installCertificate adds one: the tombstone keeps its index.
  r = internal::SyncUserCaDirectory(user, {second, c});
  CHECK(r.ok && r.written == 1 && r.removed == 0);
  CHECK(Contents(added + L"\\" + HashName(a, 2)) == AsString(c));
  CHECK(Contents(added + L"\\" + HashName(a, 0)).empty());

  // The last one leaves: it and the tombstones before it go
  // (removeUnnecessaryTombstones), the one between stays.
  r = internal::SyncUserCaDirectory(user, {second});
  CHECK(r.ok && r.written == 0 && r.removed == 1);
  CHECK(Contents(added + L"\\" + HashName(a, 2)) == "<missing>");
  CHECK(Contents(added + L"\\" + HashName(a, 1)) == AsString(second));
  CHECK(Contents(added + L"\\" + HashName(a, 0)).empty());

  // A file Gesher wrote that something else replaced is not Gesher's any more.
  Der replaced = MakeRoot("Same subject");
  CHECK(WriteBytes(added + L"\\" + HashName(a, 1), AsString(replaced)));
  // A temporary file of a writer that died before its rename.
  CHECK(WriteBytes(user + L"\\." + HashName(a, 5) + L".tmp-11-22", "partial"));

  // Every root leaves: Gesher's entries and their tombstones go; the foreign
  // and the replaced files stay; the manifest goes.
  r = internal::SyncUserCaDirectory(user, {});
  CHECK(r.ok && r.written == 0 && r.removed == 0);
  CHECK(Contents(added + L"\\" + HashName(a, 1)) == AsString(replaced));
  CHECK(Contents(foreign) == AsString(other));
  CHECK(Contents(manifest) == "<missing>");
  CHECK(List(user).count(L"." + HashName(a, 5) + L".tmp-11-22") == 0);

  // With the replaced file gone, the tombstone before it ends the run: an
  // empty index 0 is harmless (Android elides tombstones) and is reused by no
  // one; a new root goes after it.
  DeleteFileW((added + L"\\" + HashName(a, 1)).c_str());
  r = internal::SyncUserCaDirectory(user, {first});
  CHECK(r.ok && r.written == 1);
  CHECK(Contents(added + L"\\" + HashName(a, 1)) == AsString(first));
  r = internal::SyncUserCaDirectory(user, {});
  CHECK(r.ok && r.removed == 1);
  // Its removal took the tombstone before it too.
  CHECK(Contents(added + L"\\" + HashName(a, 0)) == "<missing>");
  CHECK(List(added) == std::set<std::wstring>{HashName(other, 0)});
}

// The production sync against this machine's stores, in temporary roots.
static void ThisMachine(const std::wstring& root) {
  std::wstring system = root + L"\\system";
  std::wstring data = root + L"\\data";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW(system.c_str(), nullptr);
  CreateDirectoryW(data.c_str(), nullptr);
  _putenv_s("ANDROID_ROOT", Utf8(system).c_str());
  _putenv_s("ANDROID_DATA", Utf8(data).c_str());
  gesher::net::SyncSystemCaCerts();

  std::wstring added = data + L"\\misc\\user\\0\\cacerts-added";
  std::wstring cacerts = system + L"\\etc\\security\\cacerts";
  std::set<std::string> system_certs;
  for (const std::wstring& name : List(cacerts)) system_certs.insert(Contents(cacerts + L"\\" + name));
  std::set<Sha1> program;
  internal::ReadMicrosoftRootProgram(&program);
  HCERTSTORE machine = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG |
                                         CERT_STORE_OPEN_EXISTING_FLAG,
                                     L"ROOT");
  HCERTSTORE user = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                  CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG |
                                      CERT_STORE_OPEN_EXISTING_FLAG,
                                  L"ROOT");
  size_t count = 0;
  for (const std::wstring& name : List(added)) {
    std::string der = Contents(added + L"\\" + name);
    Der bytes(der.begin(), der.end());
    count++;
    // In Windows' ROOT store, not Microsoft's.
    Sha1 thumbprint = Thumbprint(bytes);
    CRYPT_HASH_BLOB blob{20, thumbprint.data()};
    bool in_root = false;
    for (HCERTSTORE store : {machine, user}) {
      if (store == nullptr) continue;
      PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0,
                                                        CERT_FIND_SHA1_HASH, &blob, nullptr);
      if (found != nullptr) {
        in_root = true;
        CertFreeCertificateContext(found);
      }
    }
    CHECK(in_root);
    CHECK(program.count(thumbprint) == 0);
    // Still a system CA too (the system directory is unchanged).
    const uint8_t* p = bytes.data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(bytes.size()));
    CHECK(x != nullptr);
    if (x != nullptr) {
      char subject[512];
      X509_NAME_oneline(X509_get_subject_name(x), subject, sizeof(subject));
      printf("user-added on this machine: %ls %s\n", name.c_str(), subject);
      X509_free(x);
    }
    bool in_system = false;
    for (const std::string& pem : system_certs) {
      // The system files are PEM: compare by the base64 of the DER's start.
      static const char* kAlphabet =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string b64;
      for (size_t i = 0; i + 2 < bytes.size() && b64.size() < 64; i += 3) {
        uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        for (int k = 18; k >= 0; k -= 6) b64 += kAlphabet[(v >> k) & 63];
      }
      if (pem.find(b64) != std::string::npos) in_system = true;
    }
    CHECK(in_system);
  }
  if (machine != nullptr) CertCloseStore(machine, 0);
  if (user != nullptr) CertCloseStore(user, 0);
  printf("this machine: %zu user-added CA certificates\n", count);

  // GESHER_WINDOWS_CERTS=0: no Windows root, so none is user-added; Gesher's
  // entries are removed.
  _putenv_s("GESHER_WINDOWS_CERTS", "0");
  gesher::net::SyncSystemCaCerts();
  _putenv_s("GESHER_WINDOWS_CERTS", "");
  CHECK(List(added).empty());
  CHECK(Contents(data + L"\\misc\\user\\0\\.gesher-user-cacerts") == "<missing>");
}

int main() {
  wchar_t temp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return 2;
  std::wstring root = std::wstring(temp) + L"gesher-user-cacerts-" +
                      std::to_wstring(GetCurrentProcessId());
  RemoveTree(root);
  CreateDirectoryW(root.c_str(), nullptr);

  Classification();
  Directory(root + L"\\fixture");
  ThisMachine(root + L"\\machine");

  RemoveTree(root);
  if (g_failed == 0) {
    printf("user CA certificates: all passed (%d checks)\n", g_checks);
    return 0;
  }
  fprintf(stderr, "FAILED: %d/%d checks failed\n", g_failed, g_checks);
  return 1;
}
