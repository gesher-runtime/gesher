// Gesher: the system's trusted CA certificates from Windows' certificate store
// (compat/net/trust_store.cc; docs/network.md, ADR 0037).
#pragma once

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
// Failures are reported on stderr and leave the directory as it was.
void SyncSystemCaCerts();

}  // namespace gesher::net
