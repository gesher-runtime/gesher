// CI-only real profile/resource object ownership controls. No JVM substitute.
#include "../../compat/framework/installed_guest.h"
#include "../../compat/framework/restricted_launch.h"
#include "../../compat/posix/src/startup_null.h"
#include "../../compat/posix/include/gesher/system_properties.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <stdexcept>

namespace g = gesher::guest;
namespace gesher::properties { void DiagnosticSetWaitEvent(HANDLE); void DiagnosticSetInheritedMutex(HANDLE); HANDLE DiagnosticAreaMutex(); HANDLE DiagnosticHoldAreaMutex(); bool DiagnosticReleaseAreaMutex(HANDLE); }
struct OwnedEvent {
  HANDLE handle = nullptr;
  ~OwnedEvent() { if (handle && !CloseHandle(handle)) std::abort(); }
};
struct HeldPropertyMutex {
  HANDLE handle = nullptr;  // borrowed from the process-lifetime property area
  ~HeldPropertyMutex() { if (handle && !gesher::properties::DiagnosticReleaseAreaMutex(handle)) std::abort(); }
};
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
    if (argc == 7 && !wcscmp(argv[1], L"--nested")) {
      HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[4], nullptr, 10)));
      HANDLE done = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[5], nullptr, 10)));
      HANDLE inherited_mutex = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[6], nullptr, 10)));
      DWORD event_flags = 0;
      Check(GetHandleInformation(ready, &event_flags) && GetHandleInformation(done, &event_flags) &&
          GetHandleInformation(inherited_mutex, &event_flags), "inherited diagnostic handles");
      gesher::properties::DiagnosticSetInheritedMutex(inherited_mutex);
      wchar_t property_root_env[32768]{};
      DWORD root_length = GetEnvironmentVariableW(L"ANDROID_DATA", property_root_env, 32768);
      Check(root_length && root_length < 32768, "fixture property root environment");
      std::wstring readonly_path = std::wstring(property_root_env) + L"\\system\\gesher-properties\\28ccc633bf3e2ba6.area";
      HANDLE direct_write = CreateFileW(readonly_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      DWORD write_error = direct_write == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      if (direct_write != INVALID_HANDLE_VALUE) Check(CloseHandle(direct_write), "close unexpected property write handle");
      printf("POLICY_PROPERTY_ACCESS direct_write_denied=%u error=%lu\n",
          unsigned(direct_write == INVALID_HANDLE_VALUE), write_error);
      Check(direct_write == INVALID_HANDLE_VALUE && write_error == ERROR_ACCESS_DENIED, "actual area write denied");
      gesher::properties::DiagnosticSetWaitEvent(ready);
      std::string property;
      bool property_read = gesher::properties::Get("ro.fixture.identity", &property);
      std::string live;
      bool live_read = gesher::properties::Get("fixture.live", &live);
      bool property_write = gesher::properties::Set("fixture.mutable", "guest");
      printf("POLICY_PROPERTY_LIVE read=%u updated_value=%u\n", unsigned(live_read), unsigned(live == "after"));
      Check(SetEvent(done), "property read observation completed");
      printf("POLICY_PROPERTY guest_read=%u host_value=%u guest_write=%u\n", unsigned(property_read),
          unsigned(property == "host-authoritative"), unsigned(property_write));
      Check(property_read && property == "host-authoritative" && live_read && live == "after" && !property_write,
          "actual readonly live property probe");
      STARTUPINFOW actual{}; actual.cb = sizeof(actual); GetStartupInfoW(&actual);
      Check((actual.dwFlags & STARTF_USESTDHANDLES) != 0, "actual startup stdio");
      HANDLE verified = gesher::DuplicateVerifiedNull(actual.hStdInput);
      Check(verified != nullptr, "actual startup null verified");
      DWORD verified_flags = 0;
      Check(GetHandleInformation(verified, &verified_flags) && !(verified_flags & HANDLE_FLAG_INHERIT), "verified duplicate is private");
      int placeholder = _open_osfhandle(reinterpret_cast<intptr_t>(verified), _O_RDWR | _O_NOINHERIT);
      if (placeholder < 0) Check(CloseHandle(verified), "failed verified descriptor transfer close");
      Check(placeholder >= 0 && _close(placeholder) == 0, "verified descriptor transfer and close");
      HANDLE wrong = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      Check(wrong != nullptr, "wrong startup object control");
      HANDLE rejected = gesher::DuplicateVerifiedNull(wrong);
      Check(rejected == nullptr && CloseHandle(wrong), "non-null startup object rejected");
      Check(gesher::DuplicateVerifiedNull(INVALID_HANDLE_VALUE) == nullptr, "invalid startup object rejected");
      printf("POLICY_VERIFIED_NULL actual_null=1 private_duplicate=1 crt_close=1 wrong_object_rejected=1 invalid_rejected=1\n");
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
    std::wstring property_root = root + L"\\property-host";
    Directory(property_root);
    Check(_wputenv_s(L"ANDROID_DATA", property_root.c_str()) == 0 &&
        _wputenv_s(L"GESHER_BINDER", L"fixture-property-session") == 0, "property session setup");
    Check(gesher::properties::Set("ro.fixture.identity", "host-authoritative"), "host publishes real property");
    Check(gesher::properties::Set("fixture.live", "before"), "host publishes mutable property");
    std::string host_value;
    Check(gesher::properties::Get("ro.fixture.identity", &host_value) && host_value == "host-authoritative", "host reads real property");
    std::wstring asset_file = assets + L"\\payload.dat";
    HANDLE asset = CreateFileW(asset_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(asset != INVALID_HANDLE_VALUE && CloseHandle(asset), "fresh readonly asset file");
    auto outside_security = Security(outside);
    wchar_t executable[32768]; DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    Check(size && size < 32768, "actual executable");
    std::wstring property_area = property_root + L"\\system\\gesher-properties\\28ccc633bf3e2ba6.area";
    auto create = [&](const std::wstring& resource) {
      return g::InstalledPolicy::Create(g::kSelectedPackage, Generation(), 10000,
          U(std::wstring(executable, size)), {U(resource), U(asset_file), U(property_area)}, U(data));
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

    // The installed runtime directory contains subdirectories. Exercise that
    // real Gather path, not just a flat directory of files.
    const std::wstring nested_assets = assets + L"\\framework";
    Directory(nested_assets);
    const std::wstring nested_file = nested_assets + L"\\boot.art";
    HANDLE nested_asset = CreateFileW(nested_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(nested_asset != INVALID_HANDLE_VALUE && CloseHandle(nested_asset), "fresh nested runtime asset");
    auto policy = create(assets);
    Check(bool(policy), "actual provision succeeds");
    printf("POLICY_RUNTIME_NESTED actual_gather=1 nested_directory=1 nested_file=1\n");
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
      Check(gesher::launch_control::ServerMatches(private_client, GetCurrentProcessId()) &&
          !gesher::launch_control::ServerMatches(private_client, GetCurrentProcessId() + 1) &&
          !gesher::launch_control::ServerMatches(private_client, 0), "actual pipe server PID rejects wrong and zero expected identities");
      DWORD server_flags = 0, client_flags = 0;
      Check(GetHandleInformation(private_server, &server_flags) && GetHandleInformation(private_client, &client_flags) &&
          !(server_flags & HANDLE_FLAG_INHERIT) && !(client_flags & HANDLE_FLAG_INHERIT), "private endpoints not inheritable");
      std::wstring private_name = PrivateName(private_server);
      OwnedEvent ready{CreateEventW(&security, TRUE, FALSE, nullptr)};
      OwnedEvent done{CreateEventW(&security, TRUE, FALSE, nullptr)};
      Check(ready.handle && done.handle, "owned property observation events");
      OwnedEvent shared_mutex;
      Check(DuplicateHandle(GetCurrentProcess(), gesher::properties::DiagnosticAreaMutex(), GetCurrentProcess(),
          &shared_mutex.handle, SYNCHRONIZE | MUTEX_MODIFY_STATE, TRUE, 0), "limited inherited area mutex");
      std::wstring command = L"\"" + std::wstring(executable, size) + L"\" --nested \"" + data + L"\" \"" + private_name + L"\" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(ready.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(done.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(shared_mutex.handle));
      gesher::guest::RestrictedLaunch child;
      auto started = child.Start(executable, command.data(), nullptr, nullptr, startup, CREATE_NO_WINDOW, policy->sid, {log, nul, ready.handle, done.handle, shared_mutex.handle});
      Check(CloseHandle(log) && CloseHandle(nul), "close intentional parent stdio handles");
      Check(started.error == ERROR_SUCCESS, "actual lowbox nested child launch");
      HeldPropertyMutex held;
      held.handle = gesher::properties::DiagnosticHoldAreaMutex();
      Check(held.handle != nullptr, "host holds actual property mutex");
      Check(child.Resume().error == ERROR_SUCCESS, "actual lowbox nested child resume");
      Check(WaitForSingleObject(ready.handle, 5000) == WAIT_OBJECT_0, "actual guest reaches property read");
      DWORD while_held = WaitForSingleObject(done.handle, 300);
      Check(while_held == WAIT_TIMEOUT || while_held == WAIT_OBJECT_0, "bounded guest property wait");
      Check(gesher::properties::Set("fixture.live", "after"), "host updates while holding area mutex");
      Check(gesher::properties::DiagnosticReleaseAreaMutex(held.handle), "host releases actual property mutex");
      held.handle = nullptr;
      printf("POLICY_PROPERTY_MUTEX guest_blocked_while_host_held=%u inherited_limited=1\n", unsigned(while_held == WAIT_TIMEOUT));
      Check(while_held == WAIT_TIMEOUT, "actual guest waits on inherited host mutex");
      Check(WaitForSingleObject(child.process(), 15000) == WAIT_OBJECT_0, "actual lowbox nested child completion");
      bool ready_closed = CloseHandle(ready.handle) != FALSE; if (ready_closed) ready.handle = nullptr;
      bool done_closed = CloseHandle(done.handle) != FALSE; if (done_closed) done.handle = nullptr;
      bool mutex_closed = CloseHandle(shared_mutex.handle) != FALSE; if (mutex_closed) shared_mutex.handle = nullptr;
      Check(ready_closed && done_closed && mutex_closed, "actual property observation handles closed");
      DWORD code = 0; Check(GetExitCodeProcess(child.process(), &code) && code == 0 && child.Stop().confirmed, "actual nested child exit and retirement");
      Check(CloseHandle(private_server) && CloseHandle(private_client), "actual private bootstrap owner cleanup");
      HANDLE verified_log = CreateFileW(log_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      Check(verified_log != INVALID_HANDLE_VALUE, "open actual verified null child log");
      char verified_output[4096]{}; DWORD verified_size = 0; LARGE_INTEGER verified_length{};
      bool bounded_log = GetFileSizeEx(verified_log, &verified_length) && verified_length.QuadPart >= 0 &&
          verified_length.QuadPart < static_cast<LONGLONG>(sizeof(verified_output));
      bool read_log = bounded_log && ReadFile(verified_log, verified_output, static_cast<DWORD>(verified_length.QuadPart), &verified_size, nullptr) &&
          verified_size == static_cast<DWORD>(verified_length.QuadPart);
      bool closed_log = CloseHandle(verified_log) != FALSE;
      Check(bounded_log && read_log && closed_log && std::strstr(verified_output,
          "POLICY_VERIFIED_NULL actual_null=1 private_duplicate=1 crt_close=1 wrong_object_rejected=1 invalid_rejected=1"), "actual verified null child evidence");
      Check(std::strstr(verified_output, "POLICY_PROPERTY guest_read=") != nullptr, "actual property observation");
      std::string after_guest;
      bool host_saw_guest_write = gesher::properties::Get("fixture.mutable", &after_guest);
      printf("POLICY_PROPERTY_HOST observed_write=%u value_matches=%u\n", unsigned(host_saw_guest_write),
          unsigned(after_guest == "guest"));
      Check(!host_saw_guest_write, "guest cannot publish into host property area");
      printf("%s", verified_output); fflush(stdout);
      Check(DeleteFileW(log_path.c_str()), "nested child log cleanup");
      printf("POLICY_CHILD actual_lowbox=1 nested_create=1 file_reopen_read=1 nested_delete=1 exit=0 retired=1\n");
      printf("POLICY_PRIVATE actual_bootstrap=1 guest_serve_error=5 private_endpoints_inheritable=0\n");
      printf("POLICY_SERVER actual_parent_positive=1 wrong_pid_rejected=1 zero_pid_rejected=1\n");
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
    Check(DeleteFileW(nested_file.c_str()) && RemoveDirectoryW(nested_assets.c_str()), "release nested runtime asset");
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
