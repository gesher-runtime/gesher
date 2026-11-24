// Gesher: the system's trusted CA certificates from Windows' certificate store
// (compat/net/trust_store.cc; docs/network.md, ADR 0037).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <set>
#include <string>
#include <vector>

namespace gesher::net {

// Writes the system CA certificates into $ANDROID_ROOT/etc/security/cacerts,
// the directory Conscrypt's TrustedCertificateStore and the framework's
// network security config read: Android's own CA set (the cacerts directory
// next to the executable, Conscrypt's apex/ca-certificates) and the
// certificates Windows trusts as roots for TLS servers (the ROOT stores of the
// user and the machine, group policy and enterprise ones included, and
// self-signed certificates in the CA stores), less those in Windows'
// Disallowed stores. Each is a PEM file named <subject hash>.<n>, as Android
// names them. A manifest makes later calls cheap when nothing changed.
//
// Then the user-added CA certificates, $ANDROID_DATA/misc/user/0/cacerts-added
// (what Android's KeyChain writes when the user or a device admin installs a
// CA, and what TrustedCertificateStore lists as "user:" aliases): the roots of
// Windows' ROOT stores that were installed on this PC (by the user, an
// administrator, group policy, an enterprise or a program such as a content
// filter), which are neither in Microsoft's root program nor in Android's own
// CA set (LocallyInstalledRoots). Only the entries Gesher wrote are changed.
//
// Failures are reported on stderr and leave the directories as they were.
void SyncSystemCaCerts();

namespace trust_store_internal {

using Der = std::vector<uint8_t>;
using Sha1 = std::array<uint8_t, 20>;

// Microsoft's root program: the SHA-1 thumbprints listed by the AuthRoot
// certificate trust lists Windows has, the one its automatic root update keeps
// in the registry (EncodedCtl in the machine's
// SOFTWARE\Microsoft\SystemCertificates\AuthRoot\AutoUpdate key) and the one
// built into crypt32.dll (AUTHROOTSTL).
// Every root Windows distributes, or fetches into its stores on demand, is
// listed. False when neither list could be read. (The sync also counts the
// roots automatic root update marked as taken from such a list.)
bool ReadMicrosoftRootProgram(std::set<Sha1>* thumbprints);

// The roots, of `roots` (certificates of Windows' ROOT stores that may vouch
// for TLS servers), that were installed on this PC: those not listed in
// `microsoft` (ReadMicrosoftRootProgram) and not in Android's own CA set
// `android`, less the `disallowed` ones. Each once, in a stable order.
std::vector<Der> LocallyInstalledRoots(const std::vector<Der>& roots,
                                       const std::set<Sha1>& microsoft,
                                       const std::vector<Der>& android,
                                       const std::vector<Der>& disallowed);

struct UserSyncResult {
  bool ok = false;
  size_t written = 0;  // certificates added
  size_t removed = 0;  // Gesher's entries no longer wanted (deleted or tombstoned)
};

// Brings <user_dir>\cacerts-added (user_dir: a Win32 path, Android's
// /data/misc/user/<id>) to hold `certificates`, as Android's
// TrustedCertificateStore lays it out: DER files <subject hash>.<n>, a new one
// after the last of its hash, a removed one truncated to an empty tombstone
// while a later index exists and deleted (with the tombstones before it)
// otherwise. A manifest next to the directory (<user_dir>\.gesher-user-cacerts)
// names the entries Gesher wrote; entries it did not write are never changed.
// The caller serializes writers.
UserSyncResult SyncUserCaDirectory(const std::wstring& user_dir,
                                   const std::vector<Der>& certificates);

}  // namespace trust_store_internal

}  // namespace gesher::net
