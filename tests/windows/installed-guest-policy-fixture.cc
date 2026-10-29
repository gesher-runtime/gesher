// CI-only real profile/resource object ownership controls. No JVM substitute.
#include "../../compat/framework/installed_guest.h"
#include "../../compat/framework/restricted_launch.h"
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
  SetLastError(ERROR_SUCCESS);
  bool moved = MoveFileExW(path.c_str(), (path + L".moved").c_str(), 0) != FALSE;
  DWORD error = GetLastError();
  printf("POLICY_RENAME path=%ls moved=%u error=%lu\n", path.c_str(), unsigned(moved), error);
  if (moved) Check(MoveFileExW((path + L".moved").c_str(), path.c_str(), 0), "restore unexpected successful rename");
  return !moved && error == ERROR_SHARING_VIOLATION;
}
void Label(const std::wstring& path, const wchar_t* sddl) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr), "label descriptor");
  PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  Check(GetSecurityDescriptorSacl(descriptor, &present, &acl, &defaulted), "label ACL");
  HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, 0, nullptr);
  Check(handle != INVALID_HANDLE_VALUE, "label exact file");
  DWORD result = SetSecurityInfo(handle, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, acl);
  printf("POLICY_LABEL sddl=%ls result=%lu\n", sddl, result);
  Check(CloseHandle(handle), "label handle close"); LocalFree(descriptor);
  Check(result == ERROR_SUCCESS, "label exact file mutation");
}
bool SlotsEmpty() {
  for (auto& slot : g::InstalledPolicy::Slots()) if (slot.load()) return false;
  return true;
}
unsigned denied_operations = 0;
DWORD WINAPI DenySecurity(HANDLE, SE_OBJECT_TYPE, SECURITY_INFORMATION, PSID, PSID, PACL, PACL) {
  ++denied_operations; return ERROR_ACCESS_DENIED;
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
std::wstring PrivateName(HANDLE handle) {
  using Query = NTSTATUS (NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  Check(query != nullptr, "actual private object name API");
  std::vector<BYTE> buffer(65536); ULONG returned = 0;
  Check(query(handle, static_cast<OBJECT_INFORMATION_CLASS>(1), buffer.data(),
      static_cast<ULONG>(buffer.size()), &returned) >= 0, "actual private pipe name");
  auto* name = reinterpret_cast<UNICODE_STRING*>(buffer.data());
  auto begin = reinterpret_cast<uintptr_t>(buffer.data()), text = reinterpret_cast<uintptr_t>(name->Buffer);
  Check(name->Length % sizeof(wchar_t) == 0 && text >= begin && text - begin <= buffer.size() &&
      name->Length <= buffer.size() - (text - begin), "bounded private object name");
  std::wstring value(name->Buffer, name->Length / sizeof(wchar_t));
  const std::wstring prefix = L"\\Device\\NamedPipe\\";
  Check(value.compare(0, prefix.size(), prefix) == 0, "actual named pipe object type");
  return L"\\\\.\\pipe\\" + value.substr(prefix.size());
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 4 && !wcscmp(argv[1], L"--nested")) {
      HANDLE malicious = CreateNamedPipeW(argv[3], PIPE_ACCESS_DUPLEX,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, nullptr);
      DWORD pipe_error = GetLastError();
      if (malicious != INVALID_HANDLE_VALUE) CloseHandle(malicious);
      Check(malicious == INVALID_HANDLE_VALUE && pipe_error == ERROR_ACCESS_DENIED, "actual private writer pipe cannot be served by guest");
      std::wstring directory = std::wstring(argv[2]) + L"\\child-created";
      Directory(directory); Directory(directory + L"\\nested");
      std::wstring file = directory + L"\\nested\\payload";
      HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
      Check(handle != INVALID_HANDLE_VALUE, "lowbox nested file creation");
      DWORD count = 0; const char expected[] = "nested-child-data";
      Check(WriteFile(handle, expected, sizeof(expected), &count, nullptr) && count == sizeof(expected) && CloseHandle(handle), "lowbox nested file write/close");
      handle = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      char observed[sizeof(expected)]{};
      Check(handle != INVALID_HANDLE_VALUE && ReadFile(handle, observed, sizeof(observed), &count, nullptr) &&
          count == sizeof(expected) && !memcmp(expected, observed, sizeof(expected)) && CloseHandle(handle), "lowbox nested file reopen/read/close");
      Check(DeleteFileW(file.c_str()) && RemoveDirectoryW((directory + L"\\nested").c_str()) &&
          RemoveDirectoryW(directory.c_str()), "lowbox nested file/directory deletion");
      return 0;
    }
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
    printf("POLICY_REPARSE subtree_rejected=1 outside_unchanged=1 owners=0\n");

    Label(asset_file, L"S:(ML;;NW;;;LW)");
    Check(!create(assets) && SlotsEmpty(), "low label readonly asset rejected with confirmed rollback");
    Label(asset_file, L"S:(ML;;NR;;;ME)");
    Check(!create(assets) && SlotsEmpty(), "readonly asset without write-up protection rejected");
    Label(asset_file, L"S:(ML;;NW;;;ME)");
    printf("POLICY_READONLY low_label_rejected=1 missing_writeup_rejected=1 rollback_owners=0\n");

    auto policy = create(assets);
    Check(bool(policy), "actual provision succeeds");
    {
      SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
      std::wstring log_path = root + L"\\nested.log";
      HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, 0, nullptr);
      HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
      Check(log != INVALID_HANDLE_VALUE && nul != INVALID_HANDLE_VALUE, "intentional nested child stdio");
      STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = nul; startup.hStdOutput = log; startup.hStdError = log;
      HANDLE private_server = INVALID_HANDLE_VALUE, private_client = INVALID_HANDLE_VALUE;
      Check(gesher::launch_control::Channel(&private_server, &private_client), "actual OW-only private bootstrap channel");
      DWORD server_flags = 0, client_flags = 0;
      Check(GetHandleInformation(private_server, &server_flags) && GetHandleInformation(private_client, &client_flags) &&
          !(server_flags & HANDLE_FLAG_INHERIT) && !(client_flags & HANDLE_FLAG_INHERIT), "private endpoints not inheritable");
      std::wstring private_name = PrivateName(private_server);
      std::wstring command = L"\"" + std::wstring(executable, size) + L"\" --nested \"" + data + L"\" \"" + private_name + L"\"";
      gesher::guest::RestrictedLaunch child;
      auto started = child.Start(executable, command.data(), nullptr, nullptr, startup, CREATE_NO_WINDOW, policy->sid, {log, nul});
      Check(CloseHandle(log) && CloseHandle(nul), "close intentional parent stdio handles");
      Check(started.error == ERROR_SUCCESS && child.Resume().error == ERROR_SUCCESS, "actual lowbox nested child launch");
      Check(WaitForSingleObject(child.process(), 15000) == WAIT_OBJECT_0, "actual lowbox nested child completion");
      DWORD code = 0; Check(GetExitCodeProcess(child.process(), &code) && code == 0 && child.Stop().confirmed, "actual nested child exit and retirement");
      Check(CloseHandle(private_server) && CloseHandle(private_client), "actual private bootstrap owner cleanup");
      Check(DeleteFileW(log_path.c_str()), "nested child log cleanup");
      printf("POLICY_CHILD actual_lowbox=1 nested_create=1 file_reopen_read=1 nested_delete=1 exit=0 retired=1\n");
      printf("POLICY_PRIVATE actual_bootstrap=1 guest_serve_error=5 private_endpoints_inheritable=0\n");
    }
    bool root_denied = RenameDenied(root), assets_denied = RenameDenied(assets), data_denied = RenameDenied(data), file_denied = RenameDenied(asset_file);
    Check(root_denied && assets_denied && data_denied && file_denied, "ancestor and final replacement denied while pinned");
    printf("POLICY_PIN ancestor_rename=32 asset_rename=32 data_rename=32 final_file_rename=32\n");
    // Insertion AFTER enumeration must not redirect exact-handle grants or
    // cleanup. MAXIMUM_ALLOWED directories prohibit existing-child propagation.
    link = data + L"\\redirect"; Link(link, outside);
    Check(policy->Grant(data, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE), "exact root grant after hostile insertion");
    Check(Security(outside) == outside_security, "post-enumeration inserted target unchanged");
    policy->set_security_ = DenySecurity;
    Check(!policy->Retire() && denied_operations > 0, "injected security failure remains unconfirmed");
    Check(policy->retiring, "failed retirement prevents subsequent authorization");
    Check(Security(outside) == outside_security, "retirement target ACL and label untouched");
    Check(!SlotsEmpty() && RenameDenied(data), "unconfirmed retirement retains exact object");
    policy->set_security_ = SetSecurityInfo;
    Check(policy->Retire(), "actual resource cleanup retry confirmed");
    Check(Security(outside) == outside_security, "exact cleanup does not traverse inserted target");
    Check(RemoveDirectoryW(link.c_str()), "remove retirement reparse");
    policy.reset(); Check(SlotsEmpty(), "all resource owners released");
    Check(DeleteFileW(asset_file.c_str()), "release final asset file");
    Check(MoveFileExW(assets.c_str(), (assets + L".moved").c_str(), 0), "rename succeeds after confirmed retirement");
    Check(RemoveDirectoryW((assets + L".moved").c_str()) && RemoveDirectoryW(data.c_str()) &&
        RemoveDirectoryW(outside.c_str()), "exact fixture cleanup");
    printf("POLICY_RETIRE insertion_target_unchanged=1 injected_error=5 retained=1 retry_confirmed=1 postcleanup_rename=1 owners=0\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "installed policy fixture: %s win32=%lu\n", error.what(), GetLastError());
    return 1;
  }
}
