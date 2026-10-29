// CI-only real profile/resource object ownership controls. No JVM substitute.
#include "../../compat/framework/installed_guest.h"
#include <cstdio>
#include <stdexcept>

namespace g = gesher::guest;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::u16string U(const std::wstring& value) { return {value.begin(), value.end()}; }
std::u16string Generation() {
  unsigned char bytes[16];
  Check(gesher::launch_control::Random(bytes, 16), "fresh generation");
  std::u16string result;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) result += u'-';
    result += u"0123456789abcdef"[bytes[i] >> 4];
    result += u"0123456789abcdef"[bytes[i] & 15];
  }
  return result;
}
void Directory(const std::wstring& path) { Check(CreateDirectoryW(path.c_str(), nullptr), "fresh directory"); }
void Link(const std::wstring& path, const std::wstring& target) {
  Check(CreateSymbolicLinkW(path.c_str(), target.c_str(),
      SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE), "create reparse control");
}
bool RenameDenied(const std::wstring& path) {
  bool moved = MoveFileExW(path.c_str(), (path + L".moved").c_str(), 0) != FALSE;
  DWORD error = GetLastError();
  return !moved && error == ERROR_SHARING_VIOLATION;
}
bool SlotsEmpty() {
  for (auto& slot : g::InstalledPolicy::Slots()) if (slot.load()) return false;
  return true;
}
std::vector<BYTE> Security(const std::wstring& path) {
  DWORD bytes = 0;
  DWORD fields = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
  GetFileSecurityW(path.c_str(), fields, nullptr, 0, &bytes);
  Check(bytes != 0, "security snapshot size");
  std::vector<BYTE> value(bytes);
  Check(GetFileSecurityW(path.c_str(), fields, value.data(), bytes, &bytes), "security snapshot");
  return value;
}
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 2 && SlotsEmpty(), "fixture arguments and initial owners");
    std::wstring root = argv[1], assets = root + L"\\assets", data = root + L"\\data", outside = root + L"\\outside";
    Directory(assets); Directory(data); Directory(outside);
    std::wstring asset_file = assets + L"\\payload.dat";
    HANDLE asset = CreateFileW(asset_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(asset != INVALID_HANDLE_VALUE && CloseHandle(asset), "fresh readonly asset file");
    auto outside_security = Security(outside);
    wchar_t executable[32768]; DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    Check(size && size < 32768, "actual executable");
    auto create = [&](const std::wstring& resource) {
      return g::InstalledPolicy::Create(g::kSelectedPackage, Generation(), 10000,
          U(std::wstring(executable, size)), {U(resource), U(asset_file)}, U(data));
    };
    std::wstring link = assets + L"\\redirect";
    Link(link, outside);
    auto rejected = create(assets);
    Check(!rejected, "subtree reparse denied before grant");
    Check(Security(outside) == outside_security, "reparse target ACL and label untouched");
    Check(RemoveDirectoryW(link.c_str()), "remove rejected reparse");
    g::InstalledPolicy::RetryRetired();
    Check(SlotsEmpty(), "failed provision resource ownership retired after retry");
    printf("POLICY_REPARSE subtree_rejected=1 retained_cleanup_retry=1 owners=0\n");

    auto policy = create(assets);
    Check(bool(policy), "actual provision succeeds");
    Check(RenameDenied(root) && RenameDenied(assets) && RenameDenied(data) && RenameDenied(asset_file), "ancestor and final replacement denied while pinned");
    printf("POLICY_PIN ancestor_rename=32 asset_rename=32 data_rename=32 final_file_rename=32\n");
    // A fresh hostile data subtree after provisioning cannot redirect revoke.
    link = data + L"\\redirect"; Link(link, outside);
    Check(!policy->Retire(), "reparse during retirement remains unconfirmed");
    Check(policy->retiring, "failed retirement prevents subsequent authorization");
    Check(Security(outside) == outside_security, "retirement target ACL and label untouched");
    Check(!SlotsEmpty() && RenameDenied(data), "unconfirmed retirement retains exact object");
    Check(RemoveDirectoryW(link.c_str()), "remove retirement reparse");
    Check(policy->Retire(), "actual resource cleanup retry confirmed");
    policy.reset(); Check(SlotsEmpty(), "all resource owners released");
    Check(DeleteFileW(asset_file.c_str()), "release final asset file");
    Check(MoveFileExW(assets.c_str(), (assets + L".moved").c_str(), 0), "rename succeeds after confirmed retirement");
    Check(RemoveDirectoryW((assets + L".moved").c_str()) && RemoveDirectoryW(data.c_str()) &&
        RemoveDirectoryW(outside.c_str()), "exact fixture cleanup");
    printf("POLICY_RETIRE reparse_refused=1 retained=1 retry_confirmed=1 postcleanup_rename=1 owners=0\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "installed policy fixture: %s win32=%lu\n", error.what(), GetLastError());
    return 1;
  }
}
