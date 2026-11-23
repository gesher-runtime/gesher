// Gesher: experimental per-package AppContainer isolation for app processes
// (docs/decisions/0090-appcontainer-app-isolation.md, docs/sandbox.md).
//
// Off unless GESHER_SANDBOX=appcontainer. An app process then runs with a
// kernel-enforced AppContainer token of its package instead of the Windows
// user's full token: objects whose ACLs do not name its container SID or one
// of its capability SIDs are closed to it. The system server (the broker)
// creates the profile, grants the runtime's and the package's own files, and
// starts the process; binderd admits it through a capability on its pipe.
//
// Win32 only. Every function returns ERROR_SUCCESS or a Win32 error.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gesher {
namespace appcontainer {

// GESHER_SANDBOX=appcontainer in this process's environment.
bool Enabled();

// Custom capabilities Gesher's broker grants and checks (DeriveCapabilitySidsFromName).
inline constexpr const wchar_t kRuntimeFilesCapability[] = L"gesherRuntimeFiles";
inline constexpr const wchar_t kBinderClientCapability[] = L"gesherBinderClient";

// A SID allocated by these functions; freed with FreeSid or LocalFree as it came.
struct Sid {
  PSID value = nullptr;
  bool local = false;  // LocalFree (DeriveCapabilitySidsFromName), else FreeSid
  Sid() = default;
  Sid(const Sid&) = delete;
  Sid& operator=(const Sid&) = delete;
  Sid(Sid&& o) noexcept : value(o.value), local(o.local) { o.value = nullptr; }
  ~Sid();
};

// A valid profile name: 1..64 characters of [A-Za-z0-9.-] (CreateAppContainerProfile).
bool ValidName(const std::wstring& name);

// The container's profile, created for this Windows user if it does not exist.
DWORD EnsureProfile(const std::wstring& name, const std::wstring& display_name, Sid* sid);

// The SID of the container `name`, whether or not its profile exists.
DWORD ContainerSid(const std::wstring& name, Sid* sid);

// Removes the profile and its per-user folder and registry key. A missing
// profile is ERROR_SUCCESS.
DWORD DeleteContainerProfile(const std::wstring& name);

// The capability SID of a well-known ("internetClient") or custom capability name.
DWORD CapabilitySid(const std::wstring& capability, Sid* sid);

// The container's own writable folder (%LOCALAPPDATA%\Packages\<name>\AC).
DWORD FolderPath(PSID container, std::wstring* path);

// The current user's actual Windows profile path, independent of its environment.
DWORD LocalAppDataPath(std::wstring* path);

// Adds an allow ACE for `trustee` to the DACL of `path` (merged with what is
// there; inherited by children and propagated to existing ones when `inherit`).
DWORD Grant(const std::wstring& path, PSID trustee, DWORD access, bool inherit);

// The security descriptor binderd's pipe uses when the sandbox is enabled:
// SYSTEM and this user full access, the binder-client capability read and write
// but not FILE_CREATE_PIPE_INSTANCE, and a Low mandatory label (AppContainer
// processes run at Low integrity).
// Free with LocalFree.
DWORD BinderPipeSecurity(PSECURITY_DESCRIPTOR* descriptor);
// The same pipe access for a single per-run standalone AppContainer SID.
DWORD AppContainerPipeSecurity(PSECURITY_DESCRIPTOR* descriptor, PSID client);

}  // namespace appcontainer
}  // namespace gesher
