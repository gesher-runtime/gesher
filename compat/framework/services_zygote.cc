// Gesher: processes of the Gesher device, the Win32 side (see services_zygote.h).
//
// The system server starts app processes itself (a zygote, lite: Windows has
// no fork, so each is a new dalvikvm), and the first launcher starts binderd,
// servicemanager and the system server, detached: no console window, a
// process group of their own, logs in files, and no handle of the starter's
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), so that a console closing or a
// pipe a test runner reads does not depend on them.
#include <windows.h>

#include <cwchar>
#include <climits>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

#include "services_zygote.h"
#include "../posix/src/binder_wire.h"
#include "../posix/src/launch_context.h"
#include "../binder/launch_control.h"
#include "restricted_launch.h"
#include "installed_guest.h"

namespace gesher {
namespace zygote {
namespace {

std::mutex g_lock;
std::recursive_mutex g_spawn_lock; // serializes allowlist inheritance and private publication
HANDLE g_control = nullptr, g_broker = nullptr;
bool g_writer = false;
bool g_control_initialized = false;
std::atomic<bool> g_host_uid{false};
namespace control = gesher::launch_control;

void InitControl() {
  if (g_control_initialized) return;
  g_control_initialized = true;
  const auto& startup = launch::kStartup;
  if (startup.present && startup.kind == 2 && startup.uid == 1000 &&
      startup.control && startup.broker) {
    g_control = control::Handle(startup.control);
    g_broker = control::Handle(startup.broker);
    SetHandleInformation(g_control, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_broker, HANDLE_FLAG_INHERIT, 0);
    g_writer = true;
  }
}
bool Request(control::Request& request, HANDLE process = nullptr) {
  if (!g_control || !g_broker || WaitForSingleObject(g_broker, 0) != WAIT_TIMEOUT) return false;
  if (process) {
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), process, g_broker, &remote,
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, 0)) return false;
    request.process = control::Value(remote);
    request.birth = control::Birth(process);
  }
  uint64_t deadline = GetTickCount64() + 5000;
  int32_t reply = -1;
  if (control::Transfer(g_control, &request, sizeof(request), true, deadline) &&
      control::Transfer(g_control, &reply, sizeof(reply), false, deadline)) return reply == 0;
  // A partial/ambiguous request poisons the sole channel. Never continue on a
  // shifted stream or resume a child without an affirmative broker response.
  CloseHandle(g_control); g_control = nullptr;
  return false;
}
bool Identity(control::Request* request, int uid, const std::u16string& package,
              const std::u16string& install) {
  if (package.empty() || package.size() >= sizeof(request->package)) return false;
  for (size_t i = 0; i < package.size(); ++i) {
    if (package[i] > 127 || package[i] <= 32) return false;
    request->package[i] = char(package[i]);
  }
  request->uid = uid;
  return package[0] == u'@' || control::Generation(install, request->generation);
}
struct Child {
  HANDLE process;
  int64_t birth;
  std::u16string package;
  std::u16string install;
  std::shared_ptr<guest::InstalledPolicy> policy;
  std::unique_ptr<guest::RestrictedLaunch> restricted;
  // 1: an exact-handle rollback wait is active; 2: unconfirmed quarantine.
  int bootstrap_retirement = 0;
};
std::map<int, Child> g_children;  // exact retained instances, never reopened for owned queries
std::map<std::u16string, std::shared_ptr<guest::InstalledPolicy>> g_installed;
std::vector<Child> g_restricted_failed;
bool g_retiring_installed = false;
HANDLE g_job = nullptr;
std::vector<HANDLE> g_quarantined;  // failed rollback: retain until death, never authorize
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
std::atomic<int> g_fixture_rollback_faults{0};
std::atomic<int> g_fixture_rollback_pid{0};
#endif

const wchar_t* W(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

std::u16string U(const wchar_t* s, size_t n) {
  return std::u16string(reinterpret_cast<const char16_t*>(s), n);
}

// NAME of NAME=value, compared without case (Windows' environment names).
bool SameName(const std::u16string& entry, const std::u16string& name) {
  size_t eq = entry.find(u'=', 1);  // "=C:=C:\..." entries start with '='
  std::u16string n = entry.substr(0, eq);
  if (n.size() != name.size()) return false;
  return CompareStringOrdinal(W(n), static_cast<int>(n.size()), W(name),
                              static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

std::u16string Name(const std::u16string& assignment) {
  return assignment.substr(0, assignment.find(u'=', 1));
}

std::u16string EnvironmentBlock(const std::vector<std::u16string>& set,
                                const std::vector<std::u16string>& unset) {
  std::vector<std::u16string> entries;
  if (wchar_t* env = GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p != L'\0'; p += wcslen(p) + 1) {
      entries.push_back(U(p, wcslen(p)));
    }
    FreeEnvironmentStringsW(env);
  }
  auto drop = [&entries](const std::u16string& name) {
    for (auto it = entries.begin(); it != entries.end();) {
      it = SameName(*it, name) ? entries.erase(it) : it + 1;
    }
  };
  for (const auto& name : unset) drop(name);
  for (const auto& assignment : set) {
    drop(Name(assignment));
    entries.push_back(assignment);
  }
  std::u16string block;
  for (const auto& e : entries) {
    block += e;
    block.push_back(u'\0');
  }
  block.push_back(u'\0');
  return block;
}

HANDLE Job() {
  if (g_job != nullptr) return g_job;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }
  g_job = job;
  return g_job;
}

}  // namespace

static int SpawnOwned(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway, int uid, const std::u16string& package,
          const std::u16string& install, const launch::Context* bootstrap = nullptr,
          bool installed_guest = false) {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  if (uid < -1 || uid > 19999) return -ERROR_INVALID_PARAMETER;
  if (!package.empty() && (!g_control || (g_writer && package[0] == u'@')))
    return -ERROR_ACCESS_DENIED;
  std::shared_ptr<guest::InstalledPolicy> policy;
  if (installed_guest && package == guest::kSelectedPackage) {
    auto found = g_installed.find(package);
    if (g_retiring_installed || !g_writer || bootstrap || breakaway || found == g_installed.end() ||
        !GetProcessId(g_broker) || found->second->retiring || found->second->uid != uid || found->second->install != install)
      return -ERROR_ACCESS_DENIED;
    policy = found->second;
  }
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
  // Appending only: stdout and stderr share the handle, and every write goes
  // to the end, also with another process appending to the same log.
  HANDLE log = CreateFileW(W(log_path), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return -static_cast<int>(GetLastError());
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, nullptr);
  if (nul == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    CloseHandle(log);
    return -static_cast<int>(error);
  }
  std::vector<HANDLE> handles{log, nul};
  if (bootstrap && bootstrap->control) handles.push_back(control::Handle(bootstrap->control));
  if (bootstrap && bootstrap->broker) handles.push_back(control::Handle(bootstrap->broker));
  for (HANDLE handle : handles) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  int result = 0;
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                 handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
    result = -static_cast<int>(GetLastError());
  }
  if (result == 0) {
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    launch::Context context;
    if (bootstrap) context = *bootstrap;
    if (uid >= 0) {
      context.uid = static_cast<uint32_t>(uid);
      if (policy) context.server_pid = GetProcessId(g_broker);
      si.StartupInfo.cbReserved2 = sizeof(context);
      si.StartupInfo.lpReserved2 = reinterpret_cast<BYTE*>(&context);
    }
    si.lpAttributeList = attributes;
    auto child_set = set;
    uint8_t endpoint[16]{};
    if (policy) {
      wchar_t device[256]{};
      DWORD count = GetEnvironmentVariableW(L"GESHER_BINDER", device, 256);
      control::Request identity;
      if (!count || count >= 256 || !control::Generation(install, identity.generation) ||
          !control::Random(endpoint, sizeof(endpoint))) {
        DeleteProcThreadAttributeList(attributes); CloseHandle(nul); CloseHandle(log);
        return -ERROR_INVALID_DATA;
      }
      auto guest_device = control::GuestDevice(device, identity.generation, endpoint);
      child_set.push_back(u"GESHER_BINDER=" + U(guest_device.c_str(), guest_device.size()));
      child_set.push_back(u"GESHER_INSTALLED_RESTRICTED=1");
    }
    std::u16string env = EnvironmentBlock(child_set, unset);
    std::u16string cmd = command_line;  // CreateProcessW may write to it
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                  EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    PROCESS_INFORMATION pi = {};
    std::unique_ptr<guest::RestrictedLaunch> restricted;
    BOOL ok = FALSE;
    if (policy) {
      restricted = std::make_unique<guest::RestrictedLaunch>();
      auto launched = restricted->Start(W(policy->executable),
          reinterpret_cast<wchar_t*>(cmd.data()), const_cast<char16_t*>(env.data()),
          nullptr, si.StartupInfo,
          CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
          policy->sid, handles);
      if (launched.error == ERROR_SUCCESS) {
        pi.hProcess = restricted->process(); pi.dwProcessId = restricted->pid();
        ok = TRUE;
      } else {
        result = -static_cast<int>(launched.error);
        if (!launched.cleanup.confirmed) {
          std::lock_guard<std::mutex> guard(g_lock);
          g_restricted_failed.push_back(Child{restricted->process(),
              static_cast<int64_t>(control::Birth(restricted->process())), package, install,
              policy, std::move(restricted)});
        }
      }
    } else if (breakaway) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags | CREATE_BREAKAWAY_FROM_JOB,
                          const_cast<char16_t*>(env.data()), nullptr, &si.StartupInfo, &pi);
      cmd = command_line;
    }
    // A job that allows no breakaway: the process stays in it.
    if (!ok && !policy) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags, const_cast<char16_t*>(env.data()), nullptr,
                          &si.StartupInfo, &pi);
    }
    if (ok) {
      FILETIME created{}, exited{}, kernel{}, user{};
      if (pi.dwProcessId == 0 || pi.dwProcessId > INT_MAX ||
          !GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user)) {
        result = -static_cast<int>(ERROR_INVALID_DATA);
      } else {
        int64_t birth = (static_cast<int64_t>(created.dwHighDateTime) << 32) |
                        created.dwLowDateTime;
        int pid = static_cast<int>(pi.dwProcessId);
        if (policy) {
          LPWSTR sid = nullptr;
          if (ConvertSidToStringSidW(policy->sid, &sid)) {
            std::fprintf(stderr, "INSTALLED_GUEST suspended pid=%lu birth=%llu uid=%d sid=%ls capabilities=0 ownedjob=1 endpoint=%ls\n",
                pi.dwProcessId, static_cast<unsigned long long>(birth), uid, sid, control::Profile(endpoint).substr(15).c_str());
            LocalFree(sid);
          }
        }
        bool authorized = true;
        if (!package.empty()) {
          control::Request request;
          request.op = control::kPublish;
          request.restricted = policy ? 1 : 0;
          std::copy(std::begin(endpoint), std::end(endpoint), std::begin(request.endpoint));
          authorized = Identity(&request, uid, package, install) && Request(request, pi.hProcess);
          if (authorized && bootstrap && bootstrap->kind == 2) {
            control::Request handoff;
            handoff.op = control::kHandoff;
            authorized = Request(handoff, pi.hProcess);
          }
        }
        std::lock_guard<std::mutex> guard(g_lock);
        // Retain the actual CreateProcess instance before any child instruction.
        // Package/system publication already succeeded on the private channel.
        auto inserted = g_children.emplace(pid, Child{pi.hProcess, birth, package, install, policy, nullptr});
        if (!authorized || !birth || !inserted.second) {
          if (inserted.second) g_children.erase(inserted.first);
          result = -static_cast<int>(ERROR_INVALID_DATA);
        } else {
          DWORD resume_error = ERROR_SUCCESS;
          if (restricted) resume_error = restricted->Resume().error;
          else if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) resume_error = GetLastError();
          if (resume_error != ERROR_SUCCESS) {
            result = -static_cast<int>(resume_error);
            g_children.erase(inserted.first);
          } else {
            inserted.first->second.restricted = std::move(restricted);
            result = pid;
          }
        }
      }
      if (result <= 0) {
        if (!package.empty() && g_control) {
          control::Request cancel;
          cancel.op = control::kCancel;
          Request(cancel, pi.hProcess);
        }
        if (restricted) {
          auto retired = restricted->Stop();
          if (!retired.confirmed) {
            OutputDebugStringW(L"Gesher installed guest rollback unconfirmed\n");
            std::lock_guard<std::mutex> guard(g_lock);
            g_restricted_failed.push_back(Child{pi.hProcess, static_cast<int64_t>(control::Birth(pi.hProcess)),
                package, install, policy, std::move(restricted)});
          }
        } else {
          TerminateProcess(pi.hProcess, 137);
          if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
          CloseHandle(pi.hProcess);
          } else {
          // No success publication. Keep the owned instance if termination is
          // unconfirmed; never turn a failed launch into an untracked child.
          std::lock_guard<std::mutex> guard(g_lock);
          g_quarantined.push_back(pi.hProcess);
          }
        }
      }
      if (pi.hThread) CloseHandle(pi.hThread);
    } else if (!policy) {
      result = -static_cast<int>(GetLastError());
    }
    DeleteProcThreadAttributeList(attributes);
  }
  CloseHandle(nul);
  CloseHandle(log);
  if (bootstrap && bootstrap->control) SetHandleInformation(control::Handle(bootstrap->control), HANDLE_FLAG_INHERIT, 0);
  if (bootstrap && bootstrap->broker) SetHandleInformation(control::Handle(bootstrap->broker), HANDLE_FLAG_INHERIT, 0);
  return result;
}

int Spawn(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway, int uid, const std::u16string& package, const std::u16string& install,
          bool installed_guest) {
  // Public generic spawn never receives private channel/broker handles.
  return SpawnOwned(command_line, set, unset, log_path, breakaway, uid, package, install, nullptr, installed_guest);
}

bool ProvisionInstalled(const std::u16string& package, const std::u16string& install,
    int uid, const std::vector<std::u16string>& read, const std::u16string& data) {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  if (g_retiring_installed || !g_writer || !g_control || !g_broker) {
    fprintf(stderr, "installed guest policy: stage=authority retiring=%d writer=%d control=%d broker=%d\n",
        static_cast<int>(g_retiring_installed), static_cast<int>(g_writer),
        static_cast<int>(g_control != nullptr), static_cast<int>(g_broker != nullptr));
    return false;
  }
  if (package != guest::kSelectedPackage) return true;
  auto found = g_installed.find(package);
  if (found != g_installed.end()) {
    bool matches = !found->second->retiring && found->second->install == install && found->second->uid == uid;
    if (!matches) fprintf(stderr, "installed guest policy: stage=existing-mismatch\n");
    return matches;
  }
  auto policy = guest::InstalledPolicy::Create(package, install, uid, ExePath(), read, data);
  if (!policy) return false;
  g_installed.emplace(package, std::move(policy));
  return true;
}

bool VerifyRestrictedGuest() {
  const auto& startup = launch::kStartup;
  if (!startup.present || startup.kind || startup.control || startup.broker || !startup.server_pid ||
      startup.uid < 10000 || startup.uid > 19999) return false;
  HANDLE token = nullptr;
  DWORD count = 0, isolated = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  bool ok = GetTokenInformation(token, TokenIsAppContainer, &isolated, sizeof(isolated), &count) && isolated == 1;
  GetTokenInformation(token, TokenCapabilities, nullptr, 0, &count);
  std::vector<BYTE> capabilities(count);
  ok = ok && count && GetTokenInformation(token, TokenCapabilities, capabilities.data(), count, &count) &&
      reinterpret_cast<TOKEN_GROUPS*>(capabilities.data())->GroupCount == 0;
  CloseHandle(token);
  auto denied = [](const std::wstring& path, DWORD access) {
    HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return file == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED;
  };
  ok = denied(W(ExePath()), GENERIC_WRITE) && ok;
  wchar_t data[32768]{};
  DWORD length = GetEnvironmentVariableW(L"ANDROID_DATA", data, 32768);
  if (!length || length >= 32768) return false;
  std::u16string raw = U(data, length);
  if (!raw.empty() && (raw[0] == u'/' || raw[0] == u'\\')) {
    auto exe = ExePath(); raw = exe.substr(0, 2) + raw;
  }
  std::wstring root(W(raw));
  ok = denied(root + L"\\system\\packages.list", GENERIC_READ) && ok;
  ok = denied(root + L"\\system\\packages.list", GENERIC_WRITE) && ok;
  std::wstring probe = root + L"\\data\\com.gesher.lifecycle\\.restricted-proof-" + std::to_wstring(GetCurrentProcessId());
  HANDLE file = CreateFileW(probe.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
      0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const char expected[] = "installed-data";
  char observed[sizeof(expected)]{}; DWORD written = 0, read = 0;
  ok = WriteFile(file, expected, sizeof(expected), &written, nullptr) && written == sizeof(expected) && ok;
  ok = SetFilePointer(file, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER && ok;
  ok = ReadFile(file, observed, sizeof(observed), &read, nullptr) && read == sizeof(expected) &&
      std::memcmp(expected, observed, sizeof(expected)) == 0 && ok;
  ok = (CloseHandle(file) != FALSE) && ok;
  wchar_t device[512]{};
  length = GetEnvironmentVariableW(L"GESHER_BINDER", device, 512);
  if (!length || length >= 512) return false;
  std::wstring guest_device(device);
  auto suffix = guest_device.rfind(L"-guest-");
  if (suffix == std::wstring::npos) return false;
  auto cannot_serve = [](const std::wstring& name) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, nullptr);
    DWORD error = GetLastError();
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    return pipe == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED;
  };
  ok = cannot_serve(std::wstring(binder_wire::kPipePrefix) + guest_device) && ok;
  ok = cannot_serve(std::wstring(binder_wire::kPipePrefix) + guest_device.substr(0, suffix)) && ok;
  ok = denied(std::wstring(binder_wire::kPipePrefix) + guest_device.substr(0, suffix),
      FILE_GENERIC_READ | (FILE_GENERIC_WRITE & ~FILE_APPEND_DATA)) && ok;
  return ok;
}

bool IsAuthority() {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  return g_writer && g_control && g_broker && WaitForSingleObject(g_broker, 0) == WAIT_TIMEOUT;
}
bool OwnedInstall(int pid, const std::u16string& package, const std::u16string& install) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto found = g_children.find(pid);
  return found != g_children.end() && found->second.package == package &&
      found->second.install == install &&
      WaitForSingleObject(found->second.process, 0) == WAIT_TIMEOUT &&
      control::Birth(found->second.process) == static_cast<uint64_t>(found->second.birth);
}

// Called once by Launcher.startDevice. Commands contain no capabilities.
// The caller becomes an admitted host client, not a continuing registry writer.
std::vector<int> StartDevice(const std::vector<std::u16string>& commands,
                            const std::vector<std::u16string>& logs,
                            const std::vector<std::u16string>& env) {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  if (commands.size() != 3 || logs.size() != 3 || g_control || g_broker ||
      launch::kStartup.present || g_host_uid) return {};
  HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
  if (!control::Channel(&server, &client)) return {};
  launch::Context broker_start;
  broker_start.kind = 1; broker_start.control = control::Value(server);
  int broker = SpawnOwned(commands[0], env, {u"GESHER_BINDER_UID", u"GESHER_UID"},
                          logs[0], true, 0, {}, {}, &broker_start);
  CloseHandle(server);
  if (broker <= 0) { CloseHandle(client); return {}; }
  bool broker_duplicated = false;
  HANDLE rollback_broker = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto child = g_children.find(broker);
    broker_duplicated = child != g_children.end() &&
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
        !(g_fixture_rollback_faults.load() & 1) &&
#endif
        DuplicateHandle(GetCurrentProcess(), child->second.process,
        GetCurrentProcess(), &g_broker, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!broker_duplicated && child != g_children.end()) {
      child->second.bootstrap_retirement = 1;
      rollback_broker = child->second.process;
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
      g_fixture_rollback_pid.store(broker);
#endif
    }
  }
  if (!broker_duplicated) {
    CloseHandle(client);
    g_broker = nullptr;
    if (!rollback_broker) std::fprintf(stderr, "private device bootstrap: retained broker missing\n");
    if (rollback_broker) {
      // The existing map entry reserves the exact handle during the wait; no
      // allocation or PID reopen is needed even when DuplicateHandle failed.
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
      uint64_t fixture_deadline = GetTickCount64() + 5000;
      while ((g_fixture_rollback_faults.load() & 8) && GetTickCount64() < fixture_deadline) Sleep(1);
#endif
      TerminateProcess(rollback_broker, 137);
      DWORD state =
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
          (g_fixture_rollback_faults.load() & 2) ? WAIT_FAILED :
#endif
          WaitForSingleObject(rollback_broker, 5000);
      std::lock_guard<std::mutex> guard(g_lock);
      auto child = g_children.find(broker);
      if (child != g_children.end() && child->second.process == rollback_broker &&
          child->second.bootstrap_retirement == 1) {
        if (state == WAIT_OBJECT_0 &&
#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
            !(g_fixture_rollback_faults.load() & 4) &&
#endif
            CloseHandle(rollback_broker)) {
          g_children.erase(child);
        } else {
          child->second.bootstrap_retirement = 2;
          std::fprintf(stderr, "private device bootstrap: broker rollback unconfirmed\n");
        }
      }
    }
    return {};
  }
  g_control = client;
  control::Request host;
  host.op = control::kPublish;
  bool ok = Identity(&host, 2000, u"@launcher", {}) && Request(host, GetCurrentProcess());
  int manager = ok ? SpawnOwned(commands[1], env, {u"GESHER_BINDER_UID", u"GESHER_UID"},
      logs[1], true, 1000, u"@servicemanager", {}) : 0;
  launch::Context writer;
  writer.kind = 2; writer.control = control::Value(client); writer.broker = control::Value(g_broker);
  std::wstring adoption = L" --adopt " + std::to_wstring(broker) + L"," + std::to_wstring(manager);
  int system = manager > 0 ? SpawnOwned(commands[2] + U(adoption.c_str(), adoption.size()), env, {u"GESHER_BINDER_UID", u"GESHER_UID"},
      logs[2], true, 1000, u"@system_server", {}, &writer) : 0;
  if (g_control) CloseHandle(g_control);
  g_control = nullptr;
  CloseHandle(g_broker); g_broker = nullptr;
  if (system <= 0) {
    Kill(broker);
    if (manager > 0) Kill(manager);
    return {};
  }
  // Java selects this original host's honest compatibility UID only after the
  // owned record exists. It retains neither inherited control endpoint nor writer.
  g_host_uid = true;
  return {broker, manager, system};
}

#ifdef GESHER_BOOTSTRAP_ROLLBACK_FIXTURE
void FixtureRollbackFaults(int flags) { g_fixture_rollback_faults.store(flags); }
int FixtureRollbackPid() { return g_fixture_rollback_pid.load(); }
int FixtureRollbackState(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  return it == g_children.end() ? 0 : it->second.bootstrap_retirement;
}
intptr_t FixtureObserveRollback(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  HANDLE copy = nullptr;
  if (it == g_children.end() || it->second.bootstrap_retirement != 1 ||
      !DuplicateHandle(GetCurrentProcess(), it->second.process, GetCurrentProcess(), &copy,
          SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return 0;
  return reinterpret_cast<intptr_t>(copy);
}
bool FixtureReapRollback(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end() || it->second.bootstrap_retirement != 2 ||
      WaitForSingleObject(it->second.process, 0) != WAIT_OBJECT_0 || !CloseHandle(it->second.process))
    return false;
  g_children.erase(it);
  return true;
}
#endif

bool RevokeInstall(const std::u16string& package, const std::u16string& install) {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  if (package.empty() || install.empty()) return false;
  auto policy = g_installed.find(package);
  if (policy != g_installed.end() && policy->second->install == install) policy->second->retiring = true;
  if (g_writer) {
    control::Request request;
    request.op = control::kRevoke;
    // Revoke identifies package+generation; broker does not trust a UID supplied here.
    if (!Identity(&request, 0, package, install) || !Request(request)) return false;
  } else return false;
  std::lock_guard<std::mutex> guard(g_lock);
  bool ok = true;
  for (auto it = g_restricted_failed.begin(); it != g_restricted_failed.end();) {
    if (it->package == package && it->install == install) {
      if (!it->restricted->Stop().confirmed) { ok = false; ++it; }
      else it = g_restricted_failed.erase(it);
    } else ++it;
  }
  for (const auto& entry : g_children) {
    const auto& child = entry.second;
    if (child.package == package && child.install == install &&
        WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT) {
      ok = (child.restricted ? child.restricted->Stop().confirmed :
            TerminateProcess(child.process, 137) != FALSE) && ok;
    }
  }
  if (ok && package == guest::kSelectedPackage) {
    for (auto& entry : g_children) {
      auto& child = entry.second;
      if (child.package == package && child.install == install) {
        if (WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0) { ok = false; break; }
        // Release resource ownership after exact death, but retain the process
        // and its terminal DWORD for the original ExitStatus observer.
        child.policy.reset();
      }
    }
    if (ok) {
      auto policy = g_installed.find(package);
      if (policy != g_installed.end() && policy->second->install == install) {
        if (!policy->second->Retire()) ok = false;
        else g_installed.erase(policy);
      }
    }
  }
  for (auto it = g_quarantined.begin(); it != g_quarantined.end();) {
    if (WaitForSingleObject(*it, 0) == WAIT_OBJECT_0) {
      CloseHandle(*it);
      it = g_quarantined.erase(it);
    } else ++it;
  }
  for (auto it = g_children.begin(); it != g_children.end();) {
    if (it->second.bootstrap_retirement == 2) {
      if (WaitForSingleObject(it->second.process, 0) == WAIT_OBJECT_0 &&
          CloseHandle(it->second.process)) it = g_children.erase(it);
      else { ok = false; ++it; }
    } else ++it;
  }
  return ok;
}

bool RetireInstalled() {
  std::lock_guard<std::recursive_mutex> spawning(g_spawn_lock);
  InitControl();
  if (!g_writer) return false;
  g_retiring_installed = true;
  // RevokeInstall can erase the policy map. Snapshot just the trusted keys.
  std::vector<std::pair<std::u16string, std::u16string>> installs;
  for (const auto& item : g_installed) installs.emplace_back(item.first, item.second->install);
  bool ok = true;
  for (const auto& item : installs) ok = RevokeInstall(item.first, item.second) && ok;
  return ok;
}

int64_t StartTime(int pid) {
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto child = g_children.find(pid);
    if (child != g_children.end()) {
      if (child->second.bootstrap_retirement) return 0;
      return WaitForSingleObject(child->second.process, 0) == WAIT_TIMEOUT
                 ? child->second.birth : 0;
    }
  }
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                         static_cast<DWORD>(pid));
  if (h == nullptr) return 0;
  int64_t result = 0;
  FILETIME created, exited, kernel, user;
  if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT &&
      GetProcessTimes(h, &created, &exited, &kernel, &user)) {
    result = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  }
  CloseHandle(h);
  return result;
}

int64_t ExitStatus(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return kExitUnknown;
  if (it->second.bootstrap_retirement) return kExitUnknown;
  DWORD state = WaitForSingleObject(it->second.process, 0);
  if (state == WAIT_TIMEOUT) return kExitRunning;
  // Unknown API failures are never evidence of either liveness or completion.
  // Retain the exact instance so a later observer can retry.
  if (state != WAIT_OBJECT_0) return kExitUnknown;
  DWORD code = 0;
  if (!GetExitCodeProcess(it->second.process, &code)) return kExitUnknown;
  if (!it->second.restricted) CloseHandle(it->second.process);
  g_children.erase(it);
  return static_cast<int64_t>(code);
}

int ExitCode(int pid) {
  int64_t status = ExitStatus(pid);
  return status == kExitRunning ? STILL_ACTIVE : static_cast<int>(status);
}

bool Kill(int pid) {
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto child = g_children.find(pid);
    if (child != g_children.end()) {
      if (child->second.bootstrap_retirement) return false;
      return child->second.restricted ? child->second.restricted->Stop().confirmed :
          TerminateProcess(child->second.process, 137) != FALSE;
    }
  }
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  BOOL ok = TerminateProcess(h, 137);
  CloseHandle(h);
  return ok != FALSE;
}

bool JoinJob(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  HANDLE job = Job();
  if (job == nullptr) return false;
  HANDLE h = pid == 0 ? GetCurrentProcess()
                      : OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                    static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  BOOL ok = AssignProcessToJobObject(job, h);
  if (pid != 0) CloseHandle(h);
  return ok != FALSE;
}

intptr_t Lock(const std::u16string& name, int timeout_ms) {
  HANDLE m = CreateMutexW(nullptr, FALSE, W(name));
  if (m == nullptr) return 0;
  DWORD r = WaitForSingleObject(m, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  // WAIT_ABANDONED: the last holder died holding it; it is ours now.
  if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) return reinterpret_cast<intptr_t>(m);
  CloseHandle(m);
  return 0;
}

void Unlock(intptr_t handle) {
  if (handle == 0) return;
  ReleaseMutex(reinterpret_cast<HANDLE>(handle));
  CloseHandle(reinterpret_cast<HANDLE>(handle));
}

intptr_t MakeEvent(const std::u16string& name) {
  return reinterpret_cast<intptr_t>(CreateEventW(nullptr, FALSE, FALSE, W(name)));
}

bool WaitEvent(intptr_t handle, int timeout_ms) {
  if (handle == 0) return false;
  return WaitForSingleObject(reinterpret_cast<HANDLE>(handle),
                             timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)) ==
         WAIT_OBJECT_0;
}

bool SignalEvent(const std::u16string& name) {
  HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, W(name));
  if (e == nullptr) return false;
  BOOL ok = SetEvent(e);
  CloseHandle(e);
  return ok != FALSE;
}

bool WaitForBinder(const std::u16string& device, int broker_pid, int timeout_ms) {
  if (device.empty() || broker_pid <= 0 || timeout_ms < 0) return false;
  struct ScopedHandle {
    HANDLE value;
    ~ScopedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  };
  ScopedHandle broker{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, broker_pid)};
  if (!broker.value) return false;
  uint64_t deadline = GetTickCount64() + static_cast<uint64_t>(timeout_ms);
  auto remaining = [&]() -> DWORD {
    uint64_t now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
  };
  std::wstring name = binder_wire::kPipePrefix;
  name.append(reinterpret_cast<const wchar_t*>(device.data()), device.size());
  ScopedHandle pipe{INVALID_HANDLE_VALUE};
  for (;;) {
    if (WaitForSingleObject(broker.value, 0) != WAIT_TIMEOUT) return false;
    pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe.value != INVALID_HANDLE_VALUE) break;
    DWORD error = GetLastError(), left = remaining();
    if (!left || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)) return false;
    if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), std::min<DWORD>(left, 20));
    else if (WaitForSingleObject(broker.value, std::min<DWORD>(left, 10)) != WAIT_TIMEOUT) return false;
  }
  ULONG server_pid = 0;
  if (!GetNamedPipeServerProcessId(pipe.value, &server_pid) || server_pid != static_cast<ULONG>(broker_pid)) return false;
  ScopedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.value) return false;
  auto transfer = [&](void* bytes, DWORD size, bool write) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    while (size) {
      OVERLAPPED operation{};
      operation.hEvent = event.value;
      ResetEvent(event.value);
      DWORD count = 0;
      BOOL done = write ? WriteFile(pipe.value, cursor, size, &count, &operation)
                        : ReadFile(pipe.value, cursor, size, &count, &operation);
      if (!done) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[]{broker.value, event.value};
        DWORD result = WaitForMultipleObjects(2, waits, FALSE, remaining());
        if (result != WAIT_OBJECT_0 + 1) {
          CancelIoEx(pipe.value, &operation);
          GetOverlappedResult(pipe.value, &operation, &count, TRUE);
          return false;
        }
        if (!GetOverlappedResult(pipe.value, &operation, &count, FALSE)) return false;
      }
      if (!count) return false;
      size -= count;
      cursor += count;
    }
    return true;
  };
  uint32_t uid = launch::kStartup.present ? launch::kStartup.uid : g_host_uid ? 2000 : 10000;
  uint32_t words[]{7 * sizeof(uint32_t), binder_wire::kHelloProc, binder_wire::kMagic,
                    binder_wire::kVersion, GetCurrentProcessId(), uid};
  unsigned char hello[32]{};
  std::memcpy(hello, words, sizeof(words));
  uint64_t birth = control::Birth(GetCurrentProcess());
  std::memcpy(hello + sizeof(words), &birth, sizeof(birth));
  uint32_t length = 0;
  unsigned char reply[16]{};
  if (!transfer(hello, sizeof(hello), true) || !transfer(&length, sizeof(length), false)) return false;
  if (length == 12) {
    unsigned char challenge[12]{};
    uint32_t tag = 0; uint64_t locator = 0;
    if (!transfer(challenge, sizeof(challenge), false)) return false;
    std::memcpy(&tag, challenge, 4); std::memcpy(&locator, challenge + 4, 8);
    if (tag != binder_wire::kPeerChallenge) return false;
    HANDLE proof = control::Handle(locator);
    bool signalled = SetEvent(proof) != FALSE;
    CloseHandle(proof);
    uint32_t ack[]{4, binder_wire::kPeerAck};
    if (!signalled || !transfer(ack, sizeof(ack), true) || !transfer(&length, 4, false)) return false;
  }
  if (length != sizeof(reply) || !transfer(reply, sizeof(reply), false)) return false;
  int32_t status;
  uint32_t process;
  std::memcpy(&status, reply, sizeof(status));
  std::memcpy(&process, reply + 4, sizeof(process));
  return status == 0 && process != 0 && WaitForSingleObject(broker.value, 0) == WAIT_TIMEOUT;
}

std::u16string CommandLine() {
  const wchar_t* cmd = GetCommandLineW();
  return U(cmd, wcslen(cmd));
}

std::u16string ExePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return std::u16string();
    if (n < buffer.size()) return U(buffer.data(), n);
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace zygote
}  // namespace gesher
