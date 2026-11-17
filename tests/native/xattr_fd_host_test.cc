// Real Windows file/DACL/token/lifetime checks against the production backend.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <io.h>
#include <string>
#include <vector>
#include "xattr_fd_contract_test.inc"
#include "../../compat/posix/src/xattr_security_policy.h"

#define XHOST_ERROR(call, expected, message) do { errno = 0; \
  long result = (long)(call); int error = errno; \
  XCheck(result == -1 && error == (expected), message); } while (0)

static bool ApplyDacl(HANDLE file, const wchar_t* sddl) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
    return false;
  BOOL present = FALSE, defaulted = FALSE;
  PACL dacl = nullptr;
  bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present &&
            SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
  LocalFree(descriptor);
  return ok;
}

static void CurrentTokenContracts(HANDLE file, int fd) {
  HANDLE process_token = nullptr, impersonation = nullptr, restricted = nullptr;
  bool token_ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token);
  XCheck(token_ok, "open current process token for independent restricted-token test");
  if (!token_ok) return;
  DWORD needed = 0;
  GetTokenInformation(process_token, TokenUser, nullptr, 0, &needed);
  std::vector<unsigned char> storage(needed);
  token_ok = needed && GetTokenInformation(process_token, TokenUser, storage.data(), needed, &needed);
  LPWSTR owner = nullptr;
  token_ok = token_ok && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &owner);
  XCheck(token_ok, "capture the process user for an owner-only DACL");
  if (token_ok) {
    const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(owner) + L")";
    XCheck(ApplyDacl(file, sddl.c_str()), "grant file access to the process user only");
    LocalFree(owner);
    char output[32];
    XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4, "process token satisfies owner-only DACL");
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    SID_AND_ATTRIBUTES restriction{sid, 0};
    token_ok = CreateWellKnownSid(WinWorldSid, nullptr, sid, &sid_size) &&
               DuplicateToken(process_token, SecurityImpersonation, &impersonation) &&
               CreateRestrictedToken(impersonation, 0, 0, nullptr, 0, nullptr, 1, &restriction, &restricted);
    XCheck(token_ok, "create a restricted impersonation token without owner access");
    if (token_ok) {
      const bool impersonating = SetThreadToken(nullptr, restricted);
      XCheck(impersonating, "install the independent restricted thread token");
      if (impersonating) {
        XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES,
                    "admitted fd get uses current thread token");
        XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES,
                    "admitted fd set uses current thread token");
        XHOST_ERROR(flistxattr(fd, output, sizeof(output)), EACCES,
                    "admitted fd list checks current token EA rights");
        XHOST_ERROR(fremovexattr(fd, "user.base"), EACCES,
                    "admitted fd remove uses current thread token");
        XCheck(RevertToSelf(), "restore original thread token");
      }
    }
  }
  if (restricted) CloseHandle(restricted);
  if (impersonation) CloseHandle(impersonation);
  CloseHandle(process_token);
}

static void IntegrityContracts(int fd) {
  HANDLE process_token = nullptr, lower = nullptr;
  PSID low_sid = nullptr;
  bool ready = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token) &&
      DuplicateTokenEx(process_token, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT,
                       nullptr, SecurityImpersonation, TokenImpersonation, &lower) &&
      ConvertStringSidToSidW(L"S-1-16-4096", &low_sid);
  if (ready) {
    TOKEN_MANDATORY_LABEL label{{low_sid, SE_GROUP_INTEGRITY}};
    ready = SetTokenInformation(lower, TokenIntegrityLevel, &label,
                                sizeof(label) + GetLengthSid(low_sid));
  }
  XCheck(ready, "create a real lower-integrity impersonation token");
  if (ready) {
    const bool active = SetThreadToken(nullptr, lower);
    XCheck(active, "impersonate the lower-integrity caller");
    if (active) {
      char output[64];
      XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4,
             "no-write-up integrity policy preserves reads");
      XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES,
                  "current integrity policy rejects mutation through an admitted fd");
      XHOST_ERROR(fremovexattr(fd, "user.base"), EACCES,
                  "current integrity policy rejects removal through an admitted fd");
      XCheck(RevertToSelf(), "restore token after mandatory integrity checks");
    }
  }
  if (low_sid) LocalFree(low_sid);
  if (lower) CloseHandle(lower);
  if (process_token) CloseHandle(process_token);
}

static int FindAce(HANDLE file, SECURITY_INFORMATION information, bool system_acl, BYTE type) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(file, SE_FILE_OBJECT, information, nullptr, nullptr, nullptr, nullptr,
                      &descriptor) != ERROR_SUCCESS) return -1;
  BOOL present = FALSE, defaulted = FALSE;
  PACL acl = nullptr;
  const bool queried = system_acl ? GetSecurityDescriptorSacl(descriptor, &present, &acl, &defaulted)
                                 : GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted);
  int found = queried ? 0 : -1;
  if (queried && present && acl) {
    for (DWORD i = 0; i < acl->AceCount; ++i) {
      void* entry = nullptr;
      if (!GetAce(acl, i, &entry)) { found = -1; break; }
      if (static_cast<ACE_HEADER*>(entry)->AceType == type) found = 1;
    }
  }
  LocalFree(descriptor);
  return found;
}

static void RefusedPolicyOperations(int fd) {
  char output[128];
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EOPNOTSUPP,
              "delete-pending get refuses policy the fallback cannot evaluate");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EOPNOTSUPP,
              "delete-pending set refuses policy the fallback cannot evaluate");
  XHOST_ERROR(flistxattr(fd, output, sizeof(output)), EOPNOTSUPP,
              "delete-pending list refuses policy the fallback cannot evaluate");
  XHOST_ERROR(fremovexattr(fd, "user.base"), EOPNOTSUPP,
              "delete-pending remove refuses policy the fallback cannot evaluate");
}

static void PolicyBoundaryContracts(HANDLE file, int fd) {
  DWORD query_access = 0;
  QuerySecurityAccessMask(OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
      DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION |
      SCOPE_SECURITY_INFORMATION | PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION,
      &query_access);
  XCheck(query_access == READ_CONTROL, "complete fallback policy query uses only READ_CONTROL on this Windows version");
  // This constructs an in-memory native CAP-id ACE only. It does not install
  // an enterprise policy, claim a domain is available, or assert CAP enforcement.
  alignas(ACL) BYTE scope_storage[256]{};
  auto* scope = reinterpret_cast<PACL>(scope_storage);
  PSID scope_id = nullptr;
  const bool scope_ready = InitializeAcl(scope, sizeof(scope_storage), ACL_REVISION) &&
      ConvertStringSidToSidW(L"S-1-17-1", &scope_id) &&
      AddScopedPolicyIDAce(scope, ACL_REVISION, 0, 0, scope_id);
  XCheck(scope_ready, "construct an isolated native CAP identifier ACE without installing a policy");
  if (scope_ready) {
    XCheck(gesher::xattr::CheckFallbackAcl({scope_storage, scope->AclSize},
        gesher::xattr::AclKind::kSacl) == gesher::xattr::PolicySupport::kUnsupported,
        "production classifier refuses native CAP identifier metadata");
  }
  if (scope_id) LocalFree(scope_id);

  // Resource properties can be installed with WRITE_DAC; no audit-SACL access
  // or enterprise CAP deployment is needed. Read the stored ACE back by handle.
  alignas(ACL) BYTE attribute_storage[1024]{};
  auto* attributes = reinterpret_cast<PACL>(attribute_storage);
  BYTE everyone[SECURITY_MAX_SID_SIZE];
  DWORD sid_size = sizeof(everyone), used = 0;
  ULONG64 value = 42;
  wchar_t property[] = L"GesherXattrFixture";
  CLAIM_SECURITY_ATTRIBUTE_V1 attribute{};
  attribute.Name = property;
  attribute.ValueType = CLAIM_SECURITY_ATTRIBUTE_TYPE_UINT64;
  attribute.ValueCount = 1;
  attribute.Values.pUint64 = &value;
  CLAIM_SECURITY_ATTRIBUTES_INFORMATION information{};
  information.Version = CLAIM_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1;
  information.AttributeCount = 1;
  information.Attribute.pAttributeV1 = &attribute;
  const bool resource_ready = InitializeAcl(attributes, sizeof(attribute_storage), ACL_REVISION) &&
      CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sid_size) &&
      AddResourceAttributeAce(attributes, ACL_REVISION, 0, 0, everyone, &information, &used);
  XCheck(resource_ready, "construct resource metadata using the real Windows API");
  if (resource_ready) {
    const bool installed = SetSecurityInfo(file, SE_FILE_OBJECT, ATTRIBUTE_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, attributes) == ERROR_SUCCESS;
    XCheck(installed, "install a real file resource property through WRITE_DAC");
    if (installed) {
      XCheck(FindAce(file, ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION, true,
                    SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE) == 1, "native resource property is present on the held file");
      RefusedPolicyOperations(fd);
      ACL empty{};
      XCheck(InitializeAcl(&empty, sizeof(empty), ACL_REVISION) &&
          SetSecurityInfo(file, SE_FILE_OBJECT, ATTRIBUTE_SECURITY_INFORMATION,
                          nullptr, nullptr, nullptr, &empty) == ERROR_SUCCESS,
          "remove the resource property without replacing unrelated security metadata");
      XCheck(FindAce(file, ATTRIBUTE_SECURITY_INFORMATION, true, SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE) == 0,
              "native resource property is no longer present");
    }
  }

  // A redundant conditional grant exercises real conditional-ACE storage without
  // depending on a claim server. Presence still requires semantics this narrow
  // fallback does not implement, even alongside an ordinary full-access grant.
  const bool conditional = ApplyDacl(file,
      L"D:P(A;;FA;;;WD)(XA;;FR;;;WD;(Member_of {SID(S-1-1-0)}))");
  XCheck(conditional, "install a real conditional file DACL through the Windows SDDL API");
  if (conditional) {
    XCheck(FindAce(file, DACL_SECURITY_INFORMATION, false, ACCESS_ALLOWED_CALLBACK_ACE_TYPE) == 1,
            "native conditional ACE remains in the file DACL");
    RefusedPolicyOperations(fd);
  }
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore plain file authorization after policy controls");
  char output[128];
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
          "unsupported-policy calls did not modify or remove the persisted value");
  XCheck(fsetxattr(fd, "user.policy-restored", "p", 1, 0) == 0 &&
          flistxattr(fd, output, sizeof(output)) > 0 && fremovexattr(fd, "user.policy-restored") == 0,
          "simple DACL policy restores real delete-pending set/list/remove behavior");
}

static void DaclAndLifetimeContracts() {
  char temporary[MAX_PATH];
  const DWORD length = GetTempPathA(sizeof(temporary), temporary);
  XCheck(length && length < sizeof(temporary), "locate native temporary directory");
  if (!length || length >= sizeof(temporary)) return;
  char path[MAX_PATH];
  XCheck(GetTempFileNameA(temporary, "gxa", 0, path) != 0, "create native security fixture");
  HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE | WRITE_DAC | DELETE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  XCheck(file != INVALID_HANDLE_VALUE, "admit an fd before changing the DACL");
  if (file == INVALID_HANDLE_VALUE) { DeleteFileA(path); return; }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_BINARY);
  XCheck(fd >= 0, "publish native file as a real CRT descriptor");
  if (fd < 0) { CloseHandle(file); DeleteFileA(path); return; }
  PACL original_acl = nullptr;
  PSECURITY_DESCRIPTOR original = nullptr;
  DWORD status = GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, &original_acl, nullptr, &original);
  XCheck(status == ERROR_SUCCESS, "retain original DACL for cleanup");
  if (status != ERROR_SUCCESS) { close(fd); DeleteFileA(path); return; }
  XCheck(fsetxattr(fd, "user.base", "base", 4, 0) == 0, "initialize actual persistent EA");

  // Deny only file data. Native EA bits remain granted, which distinguishes
  // Android inode authorization from merely having a Windows EA handle.
  XCheck(ApplyDacl(file, L"D:P(D;;0x00000003;;;WD)(A;;FA;;;WD)"), "revoke data rights after fd admission");
  char output[256];
  XHOST_ERROR(getxattr(path, "user.base", output, sizeof(output)), EACCES, "path get requires current data-read permission");
  XHOST_ERROR(lgetxattr(path, "user.base", output, sizeof(output)), EACCES, "nofollow regular-file get requires data-read permission");
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES, "old fd cannot retain revoked data-read permission");
  XHOST_ERROR(setxattr(path, "user.base", "deny", 4, 0), EACCES, "path set requires current data-write permission");
  XHOST_ERROR(lsetxattr(path, "user.base", "deny", 4, 0), EACCES, "nofollow set requires current data-write permission");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES, "old fd cannot retain revoked data-write permission");
  XHOST_ERROR(fremovexattr(fd, "user.base"), EACCES, "old fd cannot remove after data-write revocation");
  XCheck(listxattr(path, output, sizeof(output)) > 0 && flistxattr(fd, output, sizeof(output)) > 0,
         "list requires EA rights without inventing a data-read requirement");

  XCheck(ApplyDacl(file, L"D:P(D;;0x00000008;;;WD)(A;;FA;;;WD)"), "revoke EA read while allowing data");
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES, "old fd rechecks current EA-read permission");
  XHOST_ERROR(flistxattr(fd, output, sizeof(output)), EACCES, "old fd list rechecks current EA-read permission");
  XCheck(ApplyDacl(file, L"D:P(D;;0x00000010;;;WD)(A;;FA;;;WD)"), "revoke EA write while allowing data");
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4, "EA-write denial does not forbid reading");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES, "old fd rechecks current EA-write permission");
  XHOST_ERROR(fremovexattr(fd, "user.base"), EACCES, "old fd remove rechecks EA-write permission");

  CurrentTokenContracts(file, fd);
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore access before deleting the held file");
  IntegrityContracts(fd);
  FILE_DISPOSITION_INFO pending{TRUE};
  XCheck(SetFileInformationByHandle(file, FileDispositionInfo, &pending, sizeof(pending)),
         "mark the admitted object delete-pending using Windows directly");
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "fresh authorization preserves a delete-pending admitted handle");
  XCheck(fsetxattr(fd, "user.pending", "p", 1, 0) == 0 &&
         flistxattr(fd, output, sizeof(output)) > 0 && fremovexattr(fd, "user.pending") == 0,
         "delete-pending set, list and remove remain real EA operations");
  XCheck(ApplyDacl(file, L"D:P(D;;0x00000003;;;WD)(A;;FA;;;WD)"), "revoke data rights on a delete-pending object");
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES, "delete-pending get still honors current DACL");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES, "delete-pending set still honors current DACL");
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore delete-pending fixture authorization");

  IntegrityContracts(fd);
  PolicyBoundaryContracts(file, fd);

  for (int i = 0; i < 32; ++i) fgetxattr(fd, "user.base", output, sizeof(output));
  DWORD before = 0, after = 0;
  LARGE_INTEGER start{}, end{}, frequency{};
  bool measured = GetProcessHandleCount(GetCurrentProcess(), &before) && QueryPerformanceFrequency(&frequency) &&
                  QueryPerformanceCounter(&start);
  bool values = true;
  for (int i = 0; i < 1000; ++i)
    values = fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4) && values;
  measured = QueryPerformanceCounter(&end) && GetProcessHandleCount(GetCurrentProcess(), &after) && measured;
  XCheck(values, "1000 real EA reads retain exact values");
  XCheck(measured && before == after, "current-token authorization leaks no handles across 1000 calls");
  if (measured) printf("xattr authorization measurement: 1000 gets, %.3f ms, handles %lu -> %lu\n",
                       1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart, before, after);
  XCheck(SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                         nullptr, nullptr, original_acl, nullptr) == ERROR_SUCCESS, "restore original fixture DACL");
  LocalFree(original);
  XCheck(close(fd) == 0, "retire delete-pending descriptor");
  XCheck(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
         "last close releases the delete-pending filesystem object");
}

int main() {
  xattr_fd_contracts();
  DaclAndLifetimeContracts();
  printf("xattr Windows contracts: %d checks, %d failures\n", xattr_checks, xattr_failures);
  return xattr_failures ? 1 : 0;
}
