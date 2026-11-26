// Trusted process bootstrap for Android external-storage authorization.
#pragma once

#include <string>

namespace gesher {

// Call before ART/ELF application code or constructors can change environ.
// The immutable identity is shared by all gesher_posix copies in the process.
void InitializeExternalStoragePolicy();

// App aliases and policy checks use the same frozen launch data directory.
// System tools without an app identity retain their ordinary environment.
std::wstring ExternalStorageDataRoot();

// An application's ProcessBuilder/exec environment cannot change the child's
// storage identity. A trusted system process can still launch new identities.
std::wstring ExternalStorageChildEnvironment(const wchar_t* environment);

}  // namespace gesher
