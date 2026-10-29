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
namespace gesher::properties { void DiagnosticSetWaitEvent(HANDLE); void DiagnosticSetInheritedMutex(HANDLE); HANDLE DiagnosticAreaMutex(); HANDLE DiagnosticAreaMapping(); HANDLE DiagnosticHoldAreaMutex(); bool DiagnosticReleaseAreaMutex(HANDLE); }
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
    if (argc == 11 && !wcscmp(argv[1], L"--nested")) {
      HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[4], nullptr, 10)));
      HANDLE done = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[5], nullptr, 10)));
      HANDLE inherited_mutex = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[6], nullptr, 10)));
      HANDLE guest_held = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[7], nullptr, 10)));
      HANDLE guest_release = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[8], nullptr, 10)));
      HANDLE inherited_mapping = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[9], nullptr, 10)));
      HANDLE readonly_section = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(argv[10], nullptr, 10)));
      DWORD event_flags = 0;
      Check(GetHandleInformation(ready, &event_flags) && GetHandleInformation(done, &event_flags) &&
          GetHandleInformation(inherited_mutex, &event_flags) && GetHandleInformation(guest_held, &event_flags) &&
          GetHandleInformation(guest_release, &event_flags) && GetHandleInformation(inherited_mapping, &event_flags) &&
          GetHandleInformation(readonly_section, &event_flags), "inherited diagnostic handles");
      void* readonly_view = MapViewOfFile(inherited_mapping, FILE_MAP_READ, 0, 0, 16);
      Check(readonly_view != nullptr, "actual inherited section readable");
      uint32_t mapping_magic = 0; memcpy(&mapping_magic, readonly_view, sizeof(mapping_magic));
      Check(UnmapViewOfFile(readonly_view), "inherited section read view retired");
      HANDLE elevated_mapping = nullptr;
      BOOL duplicate_write = DuplicateHandle(GetCurrentProcess(), inherited_mapping, GetCurrentProcess(),
          &elevated_mapping, FILE_MAP_WRITE, FALSE, 0);
      DWORD duplicate_error = duplicate_write ? ERROR_SUCCESS : GetLastError();
      void* writable_view = duplicate_write ? MapViewOfFile(elevated_mapping, FILE_MAP_WRITE, 0, 0, 16) : nullptr;
      DWORD view_error = writable_view ? ERROR_SUCCESS : (duplicate_write ? GetLastError() : 0);
      bool view_closed = !writable_view || UnmapViewOfFile(writable_view) != FALSE;
      bool section_closed = !duplicate_write || CloseHandle(elevated_mapping) != FALSE;
      printf("POLICY_PROPERTY_SECTION read_magic=%u duplicate_write=%u duplicate_error=%lu writable_view=%u view_error=%lu cleanup=%u\n",
          unsigned(mapping_magic == 0x47535052), unsigned(duplicate_write), duplicate_error, unsigned(writable_view != nullptr), view_error,
          unsigned(view_closed && section_closed));
      Check(mapping_magic == 0x47535052 && view_closed && section_closed, "actual inherited section observation cleanup");
      void* separate_view = MapViewOfFile(readonly_section, FILE_MAP_READ, 0, 0, 16);
      Check(separate_view != nullptr, "separate readonly section readable");
      uint32_t separate_magic = 0; memcpy(&separate_magic, separate_view, sizeof(separate_magic));
      DWORD old_protection = 0;
      BOOL protect_write = VirtualProtect(separate_view, 16, PAGE_READWRITE, &old_protection);
      DWORD protect_error = protect_write ? ERROR_SUCCESS : GetLastError();
      DWORD ignored_protection = 0;
      bool protect_restored = !protect_write || VirtualProtect(separate_view, 16, old_protection, &ignored_protection) != FALSE;
      printf("POLICY_PROPERTY_READONLY_PROTECT writable=%u error=%lu restored=%u\n", unsigned(protect_write), protect_error, unsigned(protect_restored));
      bool readonly_unmapped = UnmapViewOfFile(separate_view) != FALSE;
      Check(protect_restored && readonly_unmapped, "restore and retire separate readonly view");
      HANDLE separate_elevated = nullptr;
      BOOL separate_duplicate = DuplicateHandle(GetCurrentProcess(), readonly_section, GetCurrentProcess(),
          &separate_elevated, FILE_MAP_WRITE, FALSE, 0);
      DWORD separate_error = separate_duplicate ? ERROR_SUCCESS : GetLastError();
      void* separate_writable = separate_duplicate ? MapViewOfFile(separate_elevated, FILE_MAP_WRITE, 0, 0, 16) : nullptr;
      DWORD separate_view_error = separate_writable ? ERROR_SUCCESS : (separate_duplicate ? GetLastError() : 0);
      bool separate_view_closed = !separate_writable || UnmapViewOfFile(separate_writable) != FALSE;
      bool separate_handle_closed = !separate_duplicate || CloseHandle(separate_elevated) != FALSE;
      printf("POLICY_PROPERTY_READONLY_SECTION read_magic=%u duplicate_write=%u duplicate_error=%lu writable_view=%u view_error=%lu cleanup=%u\n",
          unsigned(separate_magic == 0x47535052), unsigned(separate_duplicate), separate_error, unsigned(separate_writable != nullptr),
          separate_view_error, unsigned(separate_view_closed && separate_handle_closed));
      Check(separate_magic == 0x47535052 && separate_view_closed && separate_handle_closed, "separate readonly section observation cleanup");
      HANDLE all_access = nullptr;
      BOOL duplicate_all = DuplicateHandle(GetCurrentProcess(), readonly_section, GetCurrentProcess(), &all_access,
          FILE_MAP_ALL_ACCESS, FALSE, 0);
      DWORD all_error = duplicate_all ? ERROR_SUCCESS : GetLastError();
      void* all_write_view = duplicate_all ? MapViewOfFile(all_access, FILE_MAP_WRITE, 0, 0, 16) : nullptr;
      DWORD all_view_error = all_write_view ? ERROR_SUCCESS : (duplicate_all ? GetLastError() : 0);
      bool all_view_closed = !all_write_view || UnmapViewOfFile(all_write_view) != FALSE;
      bool all_closed = !duplicate_all || CloseHandle(all_access) != FALSE;
      printf("POLICY_PROPERTY_READONLY_ALL duplicate=%u error=%lu writable_view=%u view_error=%lu cleanup=%u\n",
          unsigned(duplicate_all), all_error, unsigned(all_write_view != nullptr), all_view_error, unsigned(all_view_closed && all_closed));
      Check(all_view_closed && all_closed, "all-access section observation cleanup");
      HANDLE elevated_mutex = nullptr;
      BOOL elevated = DuplicateHandle(GetCurrentProcess(), inherited_mutex, GetCurrentProcess(),
          &elevated_mutex, MUTEX_ALL_ACCESS, FALSE, 0);
      DWORD elevated_error = elevated ? ERROR_SUCCESS : GetLastError();
      bool elevated_closed = !elevated || CloseHandle(elevated_mutex) != FALSE;
      printf("POLICY_PROPERTY_MUTEX_DUP requested_all_access=1 success=%u error=%lu closed=%u\n",
          unsigned(elevated), elevated_error, unsigned(elevated_closed));
      Check(elevated_closed, "close mutex escalation observation handle");
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
      gesher::properties::DiagnosticSetWaitEvent(nullptr);
      Check(_wputenv_s(L"GESHER_BINDER", L"fixture-private-guest-endpoint") == 0, "fixture changes guest endpoint");
      std::string split_value;
      bool split_read = gesher::properties::Get("ro.fixture.identity", &split_value);
      Check(_wputenv_s(L"GESHER_BINDER", L"fixture-property-session") == 0, "fixture restores guest endpoint");
      printf("POLICY_PROPERTY_SESSION_SPLIT host_value_visible=%u\n", unsigned(split_read && split_value == "host-authoritative"));
      Check(!split_read, "distinct guest endpoint does not select host area");
      DWORD acquired = WaitForSingleObject(inherited_mutex, 5000);
      Check(acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED, "guest acquires actual inherited mutex");
      HeldPropertyMutex guest_lock{inherited_mutex};
      Check(SetEvent(guest_held), "guest mutex hold acknowledged");
      Check(WaitForSingleObject(guest_release, 5000) == WAIT_OBJECT_0, "bounded guest mutex release signal");
      Check(gesher::properties::DiagnosticReleaseAreaMutex(guest_lock.handle), "guest releases actual inherited mutex");
      guest_lock.handle = nullptr;
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
      OwnedEvent guest_held{CreateEventW(&security, TRUE, FALSE, nullptr)};
      OwnedEvent guest_release{CreateEventW(&security, TRUE, FALSE, nullptr)};
      Check(guest_held.handle && guest_release.handle, "owned guest hold observation events");
      HANDLE readonly_file_raw = CreateFileW(property_area.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      Check(readonly_file_raw != INVALID_HANDLE_VALUE, "host readonly area file");
      OwnedEvent readonly_file{readonly_file_raw};
      OwnedEvent readonly_mapping{CreateFileMappingW(readonly_file.handle, &security, PAGE_READONLY, 0, 0, nullptr)};
      Check(readonly_mapping.handle != nullptr, "separate readonly area section");
      OwnedEvent shared_mapping;
      Check(DuplicateHandle(GetCurrentProcess(), gesher::properties::DiagnosticAreaMapping(), GetCurrentProcess(),
          &shared_mapping.handle, FILE_MAP_READ, TRUE, 0), "limited inherited area section");
      OwnedEvent shared_mutex;
      Check(DuplicateHandle(GetCurrentProcess(), gesher::properties::DiagnosticAreaMutex(), GetCurrentProcess(),
          &shared_mutex.handle, SYNCHRONIZE | MUTEX_MODIFY_STATE, TRUE, 0), "limited inherited area mutex");
      std::wstring command = L"\"" + std::wstring(executable, size) + L"\" --nested \"" + data + L"\" \"" + private_name + L"\" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(ready.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(done.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(shared_mutex.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(guest_held.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(guest_release.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(shared_mapping.handle)) + L" " +
          std::to_wstring(reinterpret_cast<uintptr_t>(readonly_mapping.handle));
      gesher::guest::RestrictedLaunch child;
      auto started = child.Start(executable, command.data(), nullptr, nullptr, startup, CREATE_NO_WINDOW, policy->sid, {log, nul, ready.handle, done.handle, shared_mutex.handle, guest_held.handle, guest_release.handle, shared_mapping.handle, readonly_mapping.handle});
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
      Check(WaitForSingleObject(guest_held.handle, 5000) == WAIT_OBJECT_0, "actual guest holds inherited mutex");
      HANDLE actual_host_mutex = gesher::properties::DiagnosticAreaMutex();
      Check(actual_host_mutex != nullptr, "retained host mutex available");
      DWORD host_wait = WaitForSingleObject(actual_host_mutex, 300);
      if (host_wait == WAIT_OBJECT_0 || host_wait == WAIT_ABANDONED)
        Check(gesher::properties::DiagnosticReleaseAreaMutex(actual_host_mutex), "release unexpected host acquisition");
      Check(host_wait == WAIT_TIMEOUT || host_wait == WAIT_OBJECT_0 || host_wait == WAIT_ABANDONED, "bounded host wait");
      Check(SetEvent(guest_release.handle), "release controlled guest hold");
      printf("POLICY_PROPERTY_GUEST_HOLD host_blocked=%u\n", unsigned(host_wait == WAIT_TIMEOUT));
      Check(host_wait == WAIT_TIMEOUT, "guest hold blocks host property mutex");
      Check(WaitForSingleObject(child.process(), 15000) == WAIT_OBJECT_0, "actual lowbox nested child completion");
      bool ready_closed = CloseHandle(ready.handle) != FALSE; if (ready_closed) ready.handle = nullptr;
      bool done_closed = CloseHandle(done.handle) != FALSE; if (done_closed) done.handle = nullptr;
      bool mutex_closed = CloseHandle(shared_mutex.handle) != FALSE; if (mutex_closed) shared_mutex.handle = nullptr;
      bool held_closed = CloseHandle(guest_held.handle) != FALSE; if (held_closed) guest_held.handle = nullptr;
      bool release_closed = CloseHandle(guest_release.handle) != FALSE; if (release_closed) guest_release.handle = nullptr;
      bool mapping_closed = CloseHandle(shared_mapping.handle) != FALSE; if (mapping_closed) shared_mapping.handle = nullptr;
      bool readonly_mapping_closed = CloseHandle(readonly_mapping.handle) != FALSE; if (readonly_mapping_closed) readonly_mapping.handle = nullptr;
      bool readonly_file_closed = CloseHandle(readonly_file.handle) != FALSE; if (readonly_file_closed) readonly_file.handle = nullptr;
      Check(ready_closed && done_closed && mutex_closed && held_closed && release_closed && mapping_closed &&
          readonly_mapping_closed && readonly_file_closed, "actual property observation handles closed");
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
