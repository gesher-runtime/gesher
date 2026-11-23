// Real Windows file/DACL/token/lifetime checks against the production backend.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <io.h>
#include <string>
#include <vector>
#include "xattr_fd_contract_test.inc"
#include "../../compat/posix/src/xattr_access.h"
#include "../../compat/posix/src/xattr_security_policy.h"

#define XHOST_ERROR(call, expected, message) do { errno = 0; \
  long result = (long)(call); int error = errno; \
  XCheck(result == -1 && error == (expected), message); } while (0)

static bool WinCheck(BOOL ok, const char* operation) {
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  printf("xattr Win32 control: %s result=%d error=%lu\n", operation, !!ok, error);
  XCheck(ok, operation);
  return ok;
}

static void RemoveProbe(int fd, const char* name) {
  errno = 0;
  const int result = fremovexattr(fd, name);
  const int error = errno;
  XCheck(result == 0 || (result == -1 && error == ENODATA), "retire an isolated native probe attribute");
}

static void CheckHeldAccess(HANDLE file, DWORD desired, int expected, const char* operation) {
  const int error = gesher::xattr::CheckHeldFileAccess(file, desired);
  printf("xattr held-access control: %s access=0x%08lx error=%d expected=%d\n", operation, desired, error, expected);
  XCheck(error == expected, operation);
}

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

// Error precedence follows the PR's pinned Android common kernel 964a54e5:
// admit path/fd, copy flags/name/value, authorize the inode, resolve user.*.
// The Windows EA budget is a storage limit, not Android's 65536-byte input cap.
static void ArgumentAndCapacityContracts(const char* path, int fd) {
  const char* bad = reinterpret_cast<const char*>(static_cast<uintptr_t>(1));
  char output[32];
  std::vector<char> large(65537, 'x');
  const std::string missing = std::string(path) + ".missing";
  XHOST_ERROR(setxattr(missing.c_str(), bad, bad, large.size(), 4), ENOENT,
              "declared kernel admits the path before preparing set arguments");
  XHOST_ERROR(fsetxattr(-1, bad, bad, large.size(), 4), EBADF,
              "invalid raw fd precedes set argument preparation");
  XHOST_ERROR(fsetxattr(fd, bad, bad, large.size(), 4), EINVAL,
              "unknown set flags precede name and value copying");
  XHOST_ERROR(fsetxattr(fd, "", bad, large.size(), 0), ERANGE,
              "empty name precedes oversized value");
  XHOST_ERROR(fsetxattr(fd, "unsupported.value", bad, large.size(), 0), E2BIG,
              "Android input cap precedes namespace resolution");
  XHOST_ERROR(fsetxattr(fd, "unsupported.value", bad, 1, 0), EFAULT,
              "value copying precedes namespace resolution");
  XHOST_ERROR(fsetxattr(fd, "user.", bad, 1, 0), EFAULT,
              "value copying precedes empty user suffix rejection");
  XHOST_ERROR(fsetxattr(fd, "user.capacity", bad, 65536, 0), EFAULT,
              "valid-sized input is copied before storage capacity is checked");
  XHOST_ERROR(fsetxattr(fd, "user.base", large.data(), large.size(), XATTR_CREATE), E2BIG,
              "Android oversized input precedes create collision");
  XHOST_ERROR(fsetxattr(fd, "user.base", large.data(), 65536, XATTR_CREATE), EEXIST,
              "create collision precedes backing-store capacity");
  XHOST_ERROR(fsetxattr(fd, "user.capacity", large.data(), 65536, XATTR_REPLACE), ENODATA,
              "replace-missing precedes backing-store capacity");
  XHOST_ERROR(fsetxattr(fd, "user.capacity", large.data(), 65536, 0), ENOSPC,
              "Android-valid value exceeding native EA budget returns ENOSPC");
  XHOST_ERROR(fsetxattr(fd, "user.base", large.data(), 65536, XATTR_REPLACE), ENOSPC,
              "oversized replacement returns storage exhaustion without publishing");
  XHOST_ERROR(fgetxattr(fd, "user.capacity", output, sizeof(output)), ENODATA,
              "failed capacity checks create no attribute");
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "failed large replacements preserve existing bytes");
}

static void DeniedArgumentContracts(const char* path, int fd) {
  const char* bad = reinterpret_cast<const char*>(static_cast<uintptr_t>(1));
  char output[32];
  XHOST_ERROR(getxattr(path, bad, output, sizeof(output)), EFAULT,
              "path get copies name before current data authorization");
  XHOST_ERROR(lgetxattr(path, "", output, sizeof(output)), ERANGE,
              "nofollow get rejects empty name before current data authorization");
  XHOST_ERROR(fgetxattr(fd, bad, output, sizeof(output)), EFAULT,
              "admitted fd get copies name before current data authorization");
  XHOST_ERROR(fgetxattr(fd, "", output, sizeof(output)), ERANGE,
              "admitted fd get rejects empty name before authorization");
  XHOST_ERROR(fgetxattr(fd, "user.", output, sizeof(output)), EACCES,
              "current inode authorization precedes user suffix resolution");
  XHOST_ERROR(setxattr(path, bad, bad, 65537, 4), EINVAL,
              "path set rejects flags before current data authorization");
  XHOST_ERROR(lsetxattr(path, "user.base", bad, 1, 0), EFAULT,
              "nofollow set copies input before current data authorization");
  XHOST_ERROR(fsetxattr(fd, bad, bad, 65537, 4), EINVAL,
              "admitted fd set rejects flags before current data authorization");
  XHOST_ERROR(fsetxattr(fd, "", bad, 65537, 0), ERANGE,
              "admitted fd set rejects empty name before authorization");
  XHOST_ERROR(fsetxattr(fd, "user.base", bad, 65537, 0), E2BIG,
              "admitted fd set checks Android input cap before authorization");
  XHOST_ERROR(fsetxattr(fd, "user.base", bad, 1, 0), EFAULT,
              "admitted fd set copies value before authorization");
  XHOST_ERROR(fsetxattr(fd, "user.", "x", 1, 0), EACCES,
              "set authorization precedes user suffix resolution");
  XHOST_ERROR(removexattr(path, bad), EFAULT,
              "path remove copies name before current data authorization");
  XHOST_ERROR(fremovexattr(fd, ""), ERANGE,
              "admitted fd remove rejects empty name before authorization");
  XHOST_ERROR(fremovexattr(fd, "user."), EACCES,
              "remove authorization precedes user suffix resolution");
}

static void CurrentTokenContracts(HANDLE file, int fd) {
  HANDLE process_token = nullptr, impersonation = nullptr, restricted = nullptr;
  bool token_ok = WinCheck(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token),
                          "OpenProcessToken for restricted-token fixture");
  if (!token_ok) return;
  DWORD needed = 0;
  const BOOL size_result = GetTokenInformation(process_token, TokenUser, nullptr, 0, &needed);
  const DWORD size_error = size_result ? ERROR_SUCCESS : GetLastError();
  printf("xattr Win32 control: TokenUser size result=%d error=%lu bytes=%lu\n", !!size_result, size_error, needed);
  token_ok = !size_result && size_error == ERROR_INSUFFICIENT_BUFFER && needed >= sizeof(TOKEN_USER);
  XCheck(token_ok, "query actual TokenUser storage size");
  std::vector<unsigned char> storage(needed);
  if (token_ok)
    token_ok = WinCheck(GetTokenInformation(process_token, TokenUser, storage.data(), needed, &needed), "GetTokenInformation TokenUser");
  LPWSTR owner = nullptr;
  if (token_ok)
    token_ok = WinCheck(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &owner),
                        "ConvertSidToStringSidW for owner-only DACL");
  if (token_ok) {
    const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(owner) + L")";
    XCheck(ApplyDacl(file, sddl.c_str()), "grant file access to the process user only");
    LocalFree(owner);
    char output[32];
    XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4, "process token satisfies owner-only DACL");
    CheckHeldAccess(file, FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_READ_DATA, 0,
                    "manual evaluator admits process token under owner-only DACL");
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    SID_AND_ATTRIBUTES restriction{sid, 0};
    token_ok = WinCheck(CreateWellKnownSid(WinWorldSid, nullptr, sid, &sid_size),
                        "CreateWellKnownSid for restricting SID");
    if (token_ok) {
      // DuplicateToken's returned handle has QUERY|IMPERSONATE, but the next
      // API needs DUPLICATE. Exercise that original setup error explicitly and
      // capture the real Win32 error, rather than a stale POSIX errno.
      HANDLE limited = nullptr, unexpected = nullptr;
      const bool duplicated = WinCheck(DuplicateToken(process_token, SecurityImpersonation, &limited),
                                        "DuplicateToken limited-rights negative control");
      if (duplicated) {
        const BOOL created = CreateRestrictedToken(limited, 0, 0, nullptr, 0, nullptr, 1, &restriction, &unexpected);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        printf("xattr token-rights negative control: CreateRestrictedToken result=%d error=%lu\n", !!created, error);
        XCheck(!created && error == ERROR_ACCESS_DENIED, "restricted-token creation requires TOKEN_DUPLICATE");
      }
      if (unexpected) CloseHandle(unexpected);
      if (limited) CloseHandle(limited);
      token_ok = WinCheck(DuplicateTokenEx(process_token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE,
          nullptr, SecurityImpersonation, TokenImpersonation, &impersonation),
          "DuplicateTokenEx with the rights required by CreateRestrictedToken");
    }
    if (token_ok)
      token_ok = WinCheck(CreateRestrictedToken(impersonation, 0, 0, nullptr, 0, nullptr, 1, &restriction, &restricted),
                          "CreateRestrictedToken with a real restricting SID");
    if (token_ok) {
      XCheck(IsTokenRestricted(restricted), "created token actually contains a restricting SID");
      XCheck(fsetxattr(fd, "user.token-remove", "t", 1, XATTR_CREATE) == 0,
             "seed independent restricted-token removal probe");
      const bool impersonating = WinCheck(SetThreadToken(nullptr, restricted), "SetThreadToken restricted caller");
      if (impersonating) {
        XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES,
                    "admitted fd get uses current thread token");
        XHOST_ERROR(fsetxattr(fd, "user.token-write", "deny", 4, 0), EACCES,
                    "admitted fd set uses current thread token");
        XHOST_ERROR(flistxattr(fd, output, sizeof(output)), EACCES,
                    "admitted fd list checks current token EA rights");
        XHOST_ERROR(fremovexattr(fd, "user.token-remove"), EACCES,
                    "admitted fd remove uses current thread token");
        CheckHeldAccess(file, FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_READ_DATA, EACCES,
                        "manual evaluator rejects the restricted thread token");
        WinCheck(RevertToSelf(), "RevertToSelf after restricted-token checks");
      }
      RemoveProbe(fd, "user.token-write");
      RemoveProbe(fd, "user.token-remove");
      XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
             "restricted-token probes preserve the independent base value");
    }
  }
  if (restricted) CloseHandle(restricted);
  if (impersonation) CloseHandle(impersonation);
  CloseHandle(process_token);
}

static void IntegrityContracts(HANDLE file, int fd) {
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
    XCheck(fsetxattr(fd, "user.integrity-remove", "i", 1, XATTR_CREATE) == 0,
           "seed an independent integrity-removal probe");
    const bool active = SetThreadToken(nullptr, lower);
    XCheck(active, "impersonate the lower-integrity caller");
    if (active) {
      char output[64];
      XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4,
             "no-write-up integrity policy preserves reads");
      XHOST_ERROR(fsetxattr(fd, "user.integrity-write", "deny", 4, 0), EACCES,
                  "current integrity policy rejects mutation through an admitted fd");
      XHOST_ERROR(fremovexattr(fd, "user.integrity-remove"), EACCES,
                  "current integrity policy rejects removal through an admitted fd");
      CheckHeldAccess(file, FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_READ_DATA, 0,
                      "manual evaluator preserves lower-integrity reads");
      CheckHeldAccess(file, FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_WRITE_EA | FILE_WRITE_DATA, EACCES,
                      "manual evaluator rejects lower-integrity mutation");
      XCheck(RevertToSelf(), "restore token after mandatory integrity checks");
    }
    RemoveProbe(fd, "user.integrity-write");
    RemoveProbe(fd, "user.integrity-remove");
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

enum class NativeAuthority { kKernel, kDeletePending, kFailed };

static NativeAuthority ObserveNativeAuthority(HANDLE file, DWORD desired, const char* policy, const char* operation) {
  FILE_STANDARD_INFO state{};
  const bool queried = WinCheck(GetFileInformationByHandleEx(file, FileStandardInfo, &state, sizeof(state)),
                                "FileStandardInfo before policy authorization control");
  XCheck(queried && state.DeletePending, "policy control really holds a delete-pending object");
  HANDLE reopened = INVALID_HANDLE_VALUE;
  LONG status = 0;
  // This calls the exact production native-open primitive. The fixture owns
  // the file and token throughout the immediately following public operation;
  // no mock status or test override can select the authorization path.
  const int error = gesher::xattr::ReopenFileAccess(file, desired, &reopened, &status);
  const bool opened = error == 0 && status == 0 && reopened && reopened != INVALID_HANDLE_VALUE;
  const bool pending = error == ENOENT && static_cast<ULONG>(status) == 0xc0000056 &&
                       (!reopened || reopened == INVALID_HANDLE_VALUE);
  printf("xattr native authorization control: policy=%s operation=%s delete_pending=%d status=0x%08lx error=%d authority=%s\n",
         policy, operation, !!state.DeletePending, static_cast<ULONG>(status), error,
         opened ? "kernel" : pending ? "held-security-required" : "unexpected-failure");
  XCheck(opened || pending, "native policy control either authorizes or reports STATUS_DELETE_PENDING");
  if (opened) {
    FILE_ID_INFO original_id{}, reopened_id{};
    const bool original_ok = WinCheck(GetFileInformationByHandleEx(file, FileIdInfo, &original_id, sizeof(original_id)),
                                      "FileIdInfo original policy object");
    const bool reopened_ok = WinCheck(GetFileInformationByHandleEx(reopened, FileIdInfo, &reopened_id, sizeof(reopened_id)),
                                      "FileIdInfo natively reopened policy object");
    XCheck(original_ok && reopened_ok && original_id.VolumeSerialNumber == reopened_id.VolumeSerialNumber &&
           !memcmp(&original_id.FileId, &reopened_id.FileId, sizeof(original_id.FileId)),
           "successful native authorization pins the same file identity");
  }
  if (reopened && reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
  return opened ? NativeAuthority::kKernel : pending ? NativeAuthority::kDeletePending : NativeAuthority::kFailed;
}

struct PolicyProbes {
  std::string set_name, remove_name;
  NativeAuthority set_authority = NativeAuthority::kFailed;
  NativeAuthority remove_authority = NativeAuthority::kFailed;
};

static PolicyProbes PreparePolicyProbes(int fd, const char* policy) {
  PolicyProbes probes;
  probes.set_name = std::string("user.policy-") + policy + "-set";
  probes.remove_name = std::string("user.policy-") + policy + "-remove";
  XCheck(fsetxattr(fd, probes.remove_name.c_str(), "keep", 4, XATTR_CREATE) == 0,
         "seed a separate removal value before installing unsupported policy");
  return probes;
}

static bool ContainsName(const char* list, size_t bytes, const char* name) {
  while (bytes) {
    const char* end = static_cast<const char*>(memchr(list, 0, bytes));
    if (!end) return false;
    const size_t length = static_cast<size_t>(end - list);
    if (strlen(name) == length && !memcmp(list, name, length)) return true;
    list += length + 1;
    bytes -= length + 1;
  }
  return false;
}

static void PolicyOperations(HANDLE file, int fd, const char* policy, PolicyProbes* probes) {
  constexpr DWORD common = FILE_READ_EA | FILE_READ_ATTRIBUTES;
  const struct { const char* operation; DWORD access; } cases[] = {
      {"get", common | FILE_READ_DATA}, {"set", common | FILE_WRITE_EA | FILE_WRITE_DATA},
      {"list", common}, {"remove", common | FILE_WRITE_EA | FILE_WRITE_DATA}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const NativeAuthority authority = ObserveNativeAuthority(file, cases[i].access, policy, cases[i].operation);
    // Independently require the SAME production manual evaluator to refuse the
    // real stored policy, even on a filesystem that permits native reopening.
    const int held_error = gesher::xattr::CheckHeldFileAccess(file, cases[i].access);
    printf("xattr held-security control: policy=%s operation=%s error=%d\n", policy, cases[i].operation, held_error);
    XCheck(held_error == EOPNOTSUPP, "manual held-object evaluator refuses unsupported stored policy");
    char output[256]{};
    errno = 0;
    long result;
    switch (i) {
      case 0: result = fgetxattr(fd, "user.base", output, sizeof(output)); break;
      case 1: result = fsetxattr(fd, probes->set_name.c_str(), "new", 3, XATTR_CREATE); break;
      case 2: result = flistxattr(fd, output, sizeof(output)); break;
      default: result = fremovexattr(fd, probes->remove_name.c_str()); break;
    }
    const int error = errno;
    printf("xattr public policy result: policy=%s operation=%s result=%ld error=%d\n", policy, cases[i].operation, result, error);
    if (authority == NativeAuthority::kKernel) {
      const bool value_ok = i == 0 ? result == 4 && !memcmp(output, "base", 4) :
          i == 2 ? result > 0 && static_cast<size_t>(result) <= sizeof(output) &&
                   ContainsName(output, static_cast<size_t>(result), "user.base") : result == 0;
      XCheck(value_ok, "public operation agrees with successful current-token native authorization");
    } else if (authority == NativeAuthority::kDeletePending) {
      XCheck(result == -1 && error == EOPNOTSUPP, "public fallback refuses unsupported policy after native DELETE_PENDING");
    }
    if (i == 1) probes->set_authority = authority;
    if (i == 3) probes->remove_authority = authority;
  }
}

static void FinishPolicyProbes(HANDLE file, int fd, const PolicyProbes& probes) {
  char output[256];
  CheckHeldAccess(file, FILE_READ_EA | FILE_READ_ATTRIBUTES | FILE_READ_DATA | FILE_WRITE_EA | FILE_WRITE_DATA, 0,
                  "manual evaluator admits restored simple file policy");
  if (probes.set_authority == NativeAuthority::kKernel)
    XCheck(fgetxattr(fd, probes.set_name.c_str(), output, sizeof(output)) == 3 && !memcmp(output, "new", 3),
           "kernel-authorized policy set persisted its exact independent value");
  else if (probes.set_authority == NativeAuthority::kDeletePending)
    XHOST_ERROR(fgetxattr(fd, probes.set_name.c_str(), output, sizeof(output)), ENODATA,
                "refused manual-policy set created no value");
  if (probes.remove_authority == NativeAuthority::kKernel)
    XHOST_ERROR(fgetxattr(fd, probes.remove_name.c_str(), output, sizeof(output)), ENODATA,
                "kernel-authorized policy remove actually removed its own value");
  else if (probes.remove_authority == NativeAuthority::kDeletePending)
    XCheck(fgetxattr(fd, probes.remove_name.c_str(), output, sizeof(output)) == 4 && !memcmp(output, "keep", 4),
           "refused manual-policy remove retained its exact independent value");
  RemoveProbe(fd, probes.set_name.c_str());
  RemoveProbe(fd, probes.remove_name.c_str());
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "policy probes never modify the independent base attribute");
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
    PolicyProbes probes = PreparePolicyProbes(fd, "resource");
    const bool installed = SetSecurityInfo(file, SE_FILE_OBJECT, ATTRIBUTE_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, attributes) == ERROR_SUCCESS;
    XCheck(installed, "install a real file resource property through WRITE_DAC");
    if (installed) {
      XCheck(FindAce(file, ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION, true,
                    SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE) == 1, "native resource property is present on the held file");
      PolicyOperations(file, fd, "resource", &probes);
      ACL empty{};
      XCheck(InitializeAcl(&empty, sizeof(empty), ACL_REVISION) &&
          SetSecurityInfo(file, SE_FILE_OBJECT, ATTRIBUTE_SECURITY_INFORMATION,
                          nullptr, nullptr, nullptr, &empty) == ERROR_SUCCESS,
          "remove the resource property without replacing unrelated security metadata");
      XCheck(FindAce(file, ATTRIBUTE_SECURITY_INFORMATION, true, SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE) == 0,
              "native resource property is no longer present");
    }
    FinishPolicyProbes(file, fd, probes);
  }

  // A redundant conditional grant exercises real conditional-ACE storage without
  // depending on a claim server. Presence still requires semantics this narrow
  // fallback does not implement, even alongside an ordinary full-access grant.
  PolicyProbes probes = PreparePolicyProbes(fd, "conditional");
  const bool conditional = ApplyDacl(file,
      L"D:P(A;;FA;;;WD)(XA;;FR;;;WD;(Member_of {SID(S-1-1-0)}))");
  XCheck(conditional, "install a real conditional file DACL through the Windows SDDL API");
  if (conditional) {
    XCheck(FindAce(file, DACL_SECURITY_INFORMATION, false, ACCESS_ALLOWED_CALLBACK_ACE_TYPE) == 1,
            "native conditional ACE remains in the file DACL");
    PolicyOperations(file, fd, "conditional", &probes);
  }
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore plain file authorization after policy controls");
  FinishPolicyProbes(file, fd, probes);
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
  ArgumentAndCapacityContracts(path, fd);

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
  DeniedArgumentContracts(path, fd);

  XCheck(ApplyDacl(file, L"D:P(D;;0x00000008;;;WD)(A;;FA;;;WD)"), "revoke EA read while allowing data");
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES, "old fd rechecks current EA-read permission");
  XHOST_ERROR(flistxattr(fd, output, sizeof(output)), EACCES, "old fd list rechecks current EA-read permission");
  XHOST_ERROR(getxattr(path, "", output, sizeof(output)), ERANGE,
              "path name preparation precedes current EA-read denial");
  XHOST_ERROR(fgetxattr(fd, "", output, sizeof(output)), ERANGE,
              "admitted fd name preparation precedes current EA-read denial");
  XCheck(ApplyDacl(file, L"D:P(D;;0x00000010;;;WD)(A;;FA;;;WD)"), "revoke EA write while allowing data");
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4, "EA-write denial does not forbid reading");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES, "old fd rechecks current EA-write permission");
  XHOST_ERROR(fremovexattr(fd, "user.base"), EACCES, "old fd remove rechecks EA-write permission");

  CurrentTokenContracts(file, fd);
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore access before deleting the held file");
  IntegrityContracts(file, fd);
  FILE_DISPOSITION_INFO pending{TRUE};
  WinCheck(SetFileInformationByHandle(file, FileDispositionInfo, &pending, sizeof(pending)),
           "FileDispositionInfo marks the admitted object delete-pending");
  FILE_STANDARD_INFO standard{};
  const bool state_queried = WinCheck(GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)),
                                    "FileStandardInfo verifies the disposition precondition");
  XCheck(state_queried && standard.DeletePending, "Windows actually reports the held file in its delete queue");
  XCheck(fgetxattr(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "fresh authorization preserves a delete-pending admitted handle");
  XCheck(fsetxattr(fd, "user.pending", "p", 1, 0) == 0 &&
         flistxattr(fd, output, sizeof(output)) > 0 && fremovexattr(fd, "user.pending") == 0,
         "delete-pending set, list and remove remain real EA operations");
  XCheck(ApplyDacl(file, L"D:P(D;;0x00000003;;;WD)(A;;FA;;;WD)"), "revoke data rights on a delete-pending object");
  XHOST_ERROR(fgetxattr(fd, "user.base", output, sizeof(output)), EACCES, "delete-pending get still honors current DACL");
  XHOST_ERROR(fsetxattr(fd, "user.base", "deny", 4, 0), EACCES, "delete-pending set still honors current DACL");
  XCheck(ApplyDacl(file, L"D:P(A;;FA;;;WD)"), "restore delete-pending fixture authorization");

  IntegrityContracts(file, fd);
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
  if (measured && values && before == after) printf("xattr authorization measurement: 1000 gets, %.3f ms, handles %lu -> %lu\n",
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
