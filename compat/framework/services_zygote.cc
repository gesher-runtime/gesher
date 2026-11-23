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
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "services_zygote.h"
#include "app_container.h"
#include "../binder/launch_channel.h"
#include "../posix/include/gesher/binder_endpoint.h"
#include "../posix/src/binder_wire.h"
#include "../posix/include/gesher/windows_process_identity.h"

namespace gesher {
namespace zygote {
namespace {

namespace launch = binder_launch;
namespace peer = binder_peer;

struct BootstrapHandles {
  launch::Endpoint endpoint;
  HANDLE channel;
  HANDLE broker = nullptr;
  HANDLE job_delivery = nullptr;
};

struct Enrollment {
  HANDLE channel;
  HANDLE broker;
  peer::Role role;
  uint32_t uid;
  std::array<uint8_t, 16> installation{};
  bool* channel_failed = nullptr;
};

bool SubmitLaunchRequest(HANDLE channel, launch::Request request, bool* channel_failed = nullptr) {
  uint32_t result = ERROR_ACCESS_DENIED;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  if (!launch::Transfer(channel, &request, sizeof(request), true, deadline) ||
      !launch::Transfer(channel, &result, sizeof(result), false, deadline)) {
    if (channel_failed != nullptr) *channel_failed = true;
    return false;
  }
  return result == ERROR_SUCCESS;
}

bool EnrollProcess(const Enrollment& enrollment, HANDLE process,
                   const process_identity::Process& snapshot, PSID container) {
  launch::Request request;
  request.action = launch::Action::kEnroll;
  request.role = enrollment.role;
  request.uid = enrollment.uid;
  request.pid = snapshot.pid;
  request.birth = snapshot.birth;
  request.installation = enrollment.installation;
  if (container != nullptr && !request.container.CopyFrom(container)) return false;
  if (WaitForSingleObject(enrollment.broker, 0) != WAIT_TIMEOUT) return false;
  HANDLE remote = nullptr;
  constexpr DWORD kProcessRights = SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION;
  if (!DuplicateHandle(GetCurrentProcess(), process, enrollment.broker, &remote,
                       kProcessRights, FALSE, 0)) return false;
  request.process = launch::EncodeHandle(remote);
  return SubmitLaunchRequest(enrollment.channel, request, enrollment.channel_failed);
}

std::mutex g_lock;
std::mutex g_bootstrap_lock;  // one publisher for the private control stream
std::map<int, HANDLE> g_children;  // processes this one started, for their exit codes
HANDLE g_job = nullptr;

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

HANDLE CreateLifetimeJob() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

HANDLE Job() {
  if (g_job == nullptr) g_job = CreateLifetimeJob();
  return g_job;
}

struct OwnedHandle {
  HANDLE value = nullptr;
  OwnedHandle() = default;
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  ~OwnedHandle() {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

struct HandleList {
  std::vector<char> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
  ~HandleList() { if (value != nullptr) DeleteProcThreadAttributeList(value); }

  DWORD Initialize(HANDLE* handles, size_t count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) return GetLastError();
    storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return GetLastError();
    value = list;
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles, count * sizeof(HANDLE), nullptr, nullptr)) {
      return GetLastError();
    }
    return ERROR_SUCCESS;
  }
};

}  // namespace

namespace {

// An app process's AppContainer token: its package's container SID and its
// capability SIDs, kept alive until CreateProcessW returns.
struct Container {
  appcontainer::Sid sid;
  std::vector<appcontainer::Sid> capability_sids;
  std::vector<SID_AND_ATTRIBUTES> capabilities;
  SECURITY_CAPABILITIES security{};
  std::u16string folder;
};

std::wstring Wide(const std::u16string& s) {
  return std::wstring(reinterpret_cast<const wchar_t*>(s.data()), s.size());
}

DWORD PrepareContainer(const AppContainerSpec& spec, Container* out) {
  DWORD error = appcontainer::EnsureProfile(Wide(spec.name), Wide(spec.display_name), &out->sid);
  if (error != ERROR_SUCCESS) return error;
  for (const auto& capability : spec.capabilities) {
    appcontainer::Sid sid;
    error = appcontainer::CapabilitySid(Wide(capability), &sid);
    if (error != ERROR_SUCCESS) return error;
    out->capability_sids.push_back(std::move(sid));
  }
  for (const auto& sid : out->capability_sids) {
    out->capabilities.push_back({sid.value, SE_GROUP_ENABLED});
  }
  out->security.AppContainerSid = out->sid.value;
  out->security.Capabilities = out->capabilities.empty() ? nullptr : out->capabilities.data();
  out->security.CapabilityCount = static_cast<DWORD>(out->capabilities.size());
  std::wstring folder;
  error = appcontainer::FolderPath(out->sid.value, &folder);
  if (error != ERROR_SUCCESS) return error;
  out->folder = U(folder.c_str(), folder.size());
  return ERROR_SUCCESS;
}

int SpawnImpl(const std::u16string& command_line, std::vector<std::u16string> set,
              const std::vector<std::u16string>& unset, const std::u16string& log_path,
              bool breakaway, const AppContainerSpec* sandbox,
              const BootstrapHandles* bootstrap = nullptr,
              const Enrollment* enrollment = nullptr, HANDLE lifetime_job = nullptr) {
  Container container;
  if (sandbox != nullptr) {
    DWORD error = PrepareContainer(*sandbox, &container);
    if (error != ERROR_SUCCESS) return -static_cast<int>(error);
    // Temporary files in the container's own folder: the user's %TEMP% is closed to it.
    set.push_back(u"TEMP=" + container.folder);
    set.push_back(u"TMP=" + container.folder);
  }
  const DWORD attribute_count = 1 + (sandbox != nullptr) + (lifetime_job != nullptr);
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
  peer::OwnedHandle inherited_channel;
  peer::OwnedHandle inherited_broker;
  peer::OwnedHandle inherited_job_delivery;
  launch::Startup record;
  if (bootstrap != nullptr) {
    auto make_inheritable = [](HANDLE source) -> peer::OwnedHandle {
      if (source == nullptr) return {};
      HANDLE inheritable = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &inheritable,
                           0, TRUE, DUPLICATE_SAME_ACCESS)) return {};
      return peer::OwnedHandle(inheritable);
    };
    inherited_channel = make_inheritable(bootstrap->channel);
    inherited_broker = make_inheritable(bootstrap->broker);
    inherited_job_delivery = make_inheritable(bootstrap->job_delivery);
    if ((bootstrap->channel != nullptr && !inherited_channel) ||
        (bootstrap->broker != nullptr && !inherited_broker) ||
        (bootstrap->job_delivery != nullptr && !inherited_job_delivery)) {
      DWORD error = GetLastError();
      CloseHandle(nul);
      CloseHandle(log);
      return -static_cast<int>(error == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : error);
    }
    if (inherited_channel) handles.push_back(inherited_channel.get());
    if (inherited_broker) handles.push_back(inherited_broker.get());
    if (inherited_job_delivery) handles.push_back(inherited_job_delivery.get());
    record.endpoint = bootstrap->endpoint;
    record.channel = launch::EncodeHandle(inherited_channel.get());
    record.broker = launch::EncodeHandle(inherited_broker.get());
    record.job_delivery = launch::EncodeHandle(inherited_job_delivery.get());
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);
  std::vector<char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  int result = 0;
  const bool initialized =
      InitializeProcThreadAttributeList(attributes, attribute_count, 0, &size) != FALSE;
  if (!initialized ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                 handles.size() * sizeof(HANDLE), nullptr, nullptr) ||
      (lifetime_job != nullptr &&
       !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                  &lifetime_job, sizeof(lifetime_job), nullptr, nullptr)) ||
      (sandbox != nullptr &&
       !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                  &container.security, sizeof(container.security), nullptr,
                                  nullptr))) {
    result = -static_cast<int>(GetLastError());
  }
  if (result == 0) {
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    if (bootstrap != nullptr) {
      si.StartupInfo.lpReserved2 = reinterpret_cast<BYTE*>(&record);
      si.StartupInfo.cbReserved2 = sizeof(record);
    }
    si.lpAttributeList = attributes;
    std::u16string env = EnvironmentBlock(set, unset);
    std::u16string cmd = command_line;  // CreateProcessW may write to it
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                  EXTENDED_STARTUPINFO_PRESENT;
    const bool suspended = sandbox != nullptr || enrollment != nullptr;
    if (suspended) flags |= CREATE_SUSPENDED;
    PROCESS_INFORMATION pi = {};
    BOOL ok = FALSE;
    if (breakaway) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags | CREATE_BREAKAWAY_FROM_JOB,
                          const_cast<char16_t*>(env.data()), nullptr, &si.StartupInfo, &pi);
      cmd = command_line;
    }
    // A job that allows no breakaway: the process stays in it.
    if (!ok) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags, const_cast<char16_t*>(env.data()), nullptr,
                          &si.StartupInfo, &pi);
    }
    if (ok) {
      bool retained = true;
      process_identity::Process child;
      if (suspended) {
        retained = process_identity::ReadLiveProcess(pi.hProcess, &child) &&
            child.pid == pi.dwProcessId;
        if (retained && sandbox != nullptr) {
          retained = child.principal.container.Matches(container.sid.value) &&
              child.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID;
        }
      }
      if (retained) {
        std::lock_guard<std::mutex> guard(g_lock);
        if (sandbox != nullptr && g_job != nullptr) {
          BOOL in_job = FALSE;
          retained = IsProcessInJob(pi.hProcess, g_job, &in_job) &&
                     (in_job || AssignProcessToJobObject(g_job, pi.hProcess));
        }
        if (retained) {
          result = static_cast<int>(pi.dwProcessId);
          auto previous = g_children.find(result);
          if (previous != g_children.end()) {
            CloseHandle(previous->second);
            g_children.erase(previous);
          }
          g_children.emplace(result, pi.hProcess);
          if (enrollment != nullptr) {
            retained = EnrollProcess(*enrollment, pi.hProcess, child, container.sid.value);
            if (retained && enrollment->role == peer::Role::kSystemServer) {
              launch::Request handoff;
              handoff.action = launch::Action::kHandoff;
              handoff.pid = child.pid;
              handoff.birth = child.birth;
              retained = SubmitLaunchRequest(enrollment->channel, handoff);
            }
          }
          if (retained && suspended) retained = ResumeThread(pi.hThread) == 1;
          if (!retained) {
            g_children.erase(result);
          }
        }
      }
      if (!retained) {
        TerminateProcess(pi.hProcess, ERROR_ACCESS_DENIED);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        result = -ERROR_ACCESS_DENIED;
      }
      CloseHandle(pi.hThread);
    } else {
      result = -static_cast<int>(GetLastError());
    }
  }
  if (initialized) DeleteProcThreadAttributeList(attributes);
  CloseHandle(nul);
  CloseHandle(log);
  return result;
}

}  // namespace

int Spawn(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway) {
  return SpawnImpl(command_line, set, unset, log_path, breakaway, nullptr);
}

int SpawnInAppContainer(const std::u16string& command_line,
                        const std::vector<std::u16string>& set,
                        const std::vector<std::u16string>& unset, const std::u16string& log_path,
                        const AppContainerSpec& sandbox) {
  return SpawnImpl(command_line, set, unset, log_path, false, &sandbox);
}

std::vector<int> StartProtectedDevice(const std::u16string& device,
    const std::vector<std::u16string>& commands, const std::vector<std::u16string>& logs,
    const std::vector<std::u16string>& environment) {
  std::lock_guard<std::mutex> bootstrap_lock(g_bootstrap_lock);
  process_identity::Process self;
  if (device.empty() || commands.size() != 3 || logs.size() != 3 ||
      !process_identity::ReadLiveProcess(GetCurrentProcess(), &self) || self.principal.restricted ||
      self.principal.container.size != 0 || self.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID) {
    return {};
  }

  peer::OwnedHandle lifetime_job(CreateLifetimeJob());
  if (!lifetime_job) return {};
  peer::OwnedHandle receiver;
  peer::OwnedHandle sender;
  if (!launch::PrivatePipe(&receiver, &sender)) return {};
  const BootstrapHandles broker_handles{launch::Endpoint::kBroker, receiver.get()};
  const std::vector<std::u16string> unset{u"GESHER_BINDER_UID", u"GESHER_UID"};
  const int broker = SpawnImpl(commands[0], environment, unset, logs[0], true, nullptr,
                               &broker_handles, nullptr, lifetime_job.get());
  receiver.reset();
  if (broker <= 0) return {};
  const auto stop_owned = [](int pid) {
    if (pid > 0) KillChild(pid, ChildStartTime(pid));
  };
  peer::OwnedHandle broker_handle;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto child = g_children.find(broker);
    HANDLE duplicate = nullptr;
    if (child != g_children.end()) {
      DuplicateHandle(GetCurrentProcess(), child->second, GetCurrentProcess(), &duplicate,
          SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    }
    broker_handle.reset(duplicate);
  }
  const Enrollment host{sender.get(), broker_handle.get(), peer::Role::kLauncher, 2000};
  if (!broker_handle || !EnrollProcess(host, GetCurrentProcess(), self, nullptr) ||
      !WaitForBinder(device, broker, 5000)) {
    stop_owned(broker);
    return {};
  }
  const Enrollment manager_role{sender.get(), broker_handle.get(), peer::Role::kServiceManager, 1000};
  HANDLE query_broker = nullptr;
  constexpr DWORD kPinRights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  if (!DuplicateHandle(GetCurrentProcess(), broker_handle.get(), GetCurrentProcess(),
                       &query_broker, kPinRights, FALSE, 0)) return {};
  peer::OwnedHandle manager_broker(query_broker);
  const BootstrapHandles manager_handles{launch::Endpoint::kClient, nullptr, manager_broker.get()};
  const int manager = SpawnImpl(commands[1], environment, unset, logs[1], true, nullptr, &manager_handles,
                                &manager_role, lifetime_job.get());
  if (manager <= 0) {
    stop_owned(broker);
    return {};
  }

  HANDLE read_job = nullptr;
  HANDLE write_job = nullptr;
  if (!CreatePipe(&read_job, &write_job, nullptr, 0)) return {};
  peer::OwnedHandle job_reader(read_job);
  peer::OwnedHandle job_writer(write_job);
  const BootstrapHandles server_handles{launch::Endpoint::kSystemServer, sender.get(),
                                        broker_handle.get(), job_reader.get()};
  const Enrollment system_role{sender.get(), broker_handle.get(), peer::Role::kSystemServer, 1000};
  const int system = SpawnImpl(commands[2], environment, unset, logs[2], true, nullptr,
      &server_handles, &system_role, lifetime_job.get());
  job_reader.reset();
  if (system <= 0) {
    stop_owned(manager);
    stop_owned(broker);
    return {};
  }

  // The launcher owns the only job handle while the system server is suspended.
  // Deliver a non-inheritable copy to that exact running child, then drop ours.
  HANDLE remote_job = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto child = g_children.find(system);
    if (child != g_children.end()) {
      DuplicateHandle(GetCurrentProcess(), lifetime_job.get(), child->second, &remote_job,
                      0, FALSE, DUPLICATE_SAME_ACCESS);
    }
  }
  uint64_t delivered = launch::EncodeHandle(remote_job);
  if (remote_job == nullptr || !launch::Transfer(job_writer.get(), &delivered, sizeof(delivered), true)) {
    TerminateJobObject(lifetime_job.get(), ERROR_ACCESS_DENIED);
    return {};
  }
  return {broker, manager, system};
}

namespace {

struct Writer {
  peer::OwnedHandle channel;
  peer::OwnedHandle broker;
};

Writer ReadWriter() {
  launch::Startup startup;
  process_identity::Process self;
  if (!launch::ReadStartup(launch::Endpoint::kSystemServer, &startup) ||
      !process_identity::ReadLiveProcess(GetCurrentProcess(), &self) || self.principal.restricted ||
      self.principal.container.size != 0 || self.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID) {
    return {};
  }
  Writer writer{peer::OwnedHandle(launch::DecodeHandle(startup.channel)),
                peer::OwnedHandle(launch::DecodeHandle(startup.broker))};
  peer::OwnedHandle job_pipe(launch::DecodeHandle(startup.job_delivery));
  if (!SetHandleInformation(writer.channel.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(writer.broker.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(job_pipe.get(), HANDLE_FLAG_INHERIT, 0)) return {};
  uint64_t delivered = 0;
  if (!launch::Transfer(job_pipe.get(), &delivered, sizeof(delivered), false)) return {};
  peer::OwnedHandle job(launch::DecodeHandle(delivered));
  BOOL in_job = FALSE;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if (!job || !IsProcessInJob(GetCurrentProcess(), job.get(), &in_job) || !in_job ||
      !QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits), nullptr) ||
      (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0) return {};
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_job != nullptr) return {};
    g_job = job.release();
  }
  return writer;
}

Writer& Authority() {
  static Writer authority = ReadWriter();
  return authority;
}

}  // namespace

bool HasProtectedLaunchAuthority() {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  return authority.channel && authority.broker &&
         WaitForSingleObject(authority.broker.get(), 0) == WAIT_TIMEOUT;
}

bool PrepareProtectedOperator(int broker_pid, uint64_t birth) {
  return broker_pid > 0 && binder_endpoint::Current().PrepareOperator(
      static_cast<DWORD>(broker_pid), birth);
}

int SpawnProtectedApp(const std::u16string& command_line, const std::vector<std::u16string>& set,
    const std::vector<std::u16string>& unset, const std::u16string& log_path,
    const AppContainerSpec& sandbox, uint32_t uid, const std::array<uint8_t, 16>& installation) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  if (!authority.channel || !authority.broker || uid < 10000 || uid > 19999 ||
      installation == std::array<uint8_t, 16>{}) return -ERROR_ACCESS_DENIED;
  HANDLE query_broker = nullptr;
  constexpr DWORD kPinRights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  if (!DuplicateHandle(GetCurrentProcess(), authority.broker.get(), GetCurrentProcess(),
                       &query_broker, kPinRights, FALSE, 0)) return -ERROR_ACCESS_DENIED;
  peer::OwnedHandle app_broker(query_broker);
  const BootstrapHandles app_handles{launch::Endpoint::kClient, nullptr, app_broker.get()};
  bool channel_failed = false;
  const Enrollment enrollment{authority.channel.get(), authority.broker.get(),
                              peer::Role::kApplication, uid, installation, &channel_failed};
  int pid = SpawnImpl(command_line, set, unset, log_path, false, &sandbox, &app_handles, &enrollment);
  if (channel_failed) authority = {};
  return pid;
}

bool ChangeProtectedInstallation(const std::array<uint8_t, 16>& installation, bool revoke) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  if (!authority.channel || !authority.broker || installation == std::array<uint8_t, 16>{}) {
    return false;
  }
  launch::Request request;
  request.action = revoke ? launch::Action::kRevokeInstallation : launch::Action::kRestoreInstallation;
  request.installation = installation;
  bool channel_failed = false;
  bool accepted = SubmitLaunchRequest(authority.channel.get(), request, &channel_failed);
  if (channel_failed) authority = {};
  return accepted;
}

uint32_t EnsureAppContainer(const std::u16string& name, const std::u16string& display_name) {
  appcontainer::Sid sid;
  return appcontainer::EnsureProfile(Wide(name), Wide(display_name), &sid);
}

uint32_t GrantToAppContainer(const std::u16string& path, const std::u16string& name,
                             uint32_t access, bool inherit) {
  appcontainer::Sid sid;
  DWORD error = appcontainer::ContainerSid(Wide(name), &sid);
  if (error != ERROR_SUCCESS) return error;
  return appcontainer::Grant(Wide(path), sid.value, access, inherit);
}

uint32_t GrantToCapability(const std::u16string& path, const std::u16string& capability,
                           uint32_t access, bool inherit) {
  appcontainer::Sid sid;
  DWORD error = appcontainer::CapabilitySid(Wide(capability), &sid);
  if (error != ERROR_SUCCESS) return error;
  return appcontainer::Grant(Wide(path), sid.value, access, inherit);
}

uint32_t DeleteAppContainer(const std::u16string& name) {
  return appcontainer::DeleteContainerProfile(Wide(name));
}

uint32_t RestartWithUid(int uid, uint32_t* exit_code) {
  if (uid < 10000 || exit_code == nullptr) return ERROR_INVALID_PARAMETER;
  std::u16string exe = ExePath(), command = CommandLine();
  if (exe.empty() || command.empty()) return ERROR_INVALID_PARAMETER;
  // This is a loop guard, never evidence of package admission. The new VM parses
  // and admits the APK again and checks its native/Binder UID before binding it.
  const std::wstring decimal = std::to_wstring(uid);
  const std::u16string identity = U(decimal.c_str(), decimal.size());
  std::u16string environment = EnvironmentBlock(
      {u"GESHER_UID=" + identity, u"GESHER_BINDER_UID=" + identity,
       u"GESHER_STANDALONE_RESTARTED=1"}, {});

  OwnedHandle input, output, error, job, process, thread;
  const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  OwnedHandle* copies[] = {&input, &output, &error};
  HANDLE handles[3]{};
  for (size_t i = 0; i < 3; ++i) {
    HANDLE original = GetStdHandle(kinds[i]);
    if (original == nullptr || original == INVALID_HANDLE_VALUE) {
      SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
      copies[i]->value = CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, 0, nullptr);
      if (copies[i]->value == INVALID_HANDLE_VALUE) return GetLastError();
    } else if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                                &copies[i]->value, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
      return GetLastError();
    }
    handles[i] = copies[i]->value;
  }
  HandleList attributes;
  DWORD failure = attributes.Initialize(handles, 3);
  if (failure != ERROR_SUCCESS) return failure;
  job.value = CreateJobObjectW(nullptr, nullptr);
  if (job.value == nullptr) return GetLastError();
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) return GetLastError();

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = error.value;
  startup.lpAttributeList = attributes.value;
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(W(exe), reinterpret_cast<wchar_t*>(command.data()), nullptr, nullptr,
                       TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                             EXTENDED_STARTUPINFO_PRESENT,
                       environment.data(), nullptr, &startup.StartupInfo, &child)) {
    return GetLastError();
  }
  process.value = child.hProcess;
  thread.value = child.hThread;
  // Bind lifetime ownership before the child can execute any runtime or APK code.
  if (!AssignProcessToJobObject(job.value, process.value)) {
    failure = GetLastError();
    if (TerminateProcess(process.value, 1)) WaitForSingleObject(process.value, INFINITE);
    return failure;
  }
  // Transfer only the foreground permission Windows actually gives this launcher.
  // Failure leaves the normal OS focus rules in force; it never fabricates focus.
  AllowSetForegroundWindow(child.dwProcessId);
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return GetLastError();
  if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) return GetLastError();
  DWORD code = 0;
  if (!GetExitCodeProcess(process.value, &code)) return GetLastError();
  *exit_code = code;
  return ERROR_SUCCESS;
}

int64_t StartTime(int pid) {
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

int ExitCode(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return -1;
  DWORD code = 0;
  if (!GetExitCodeProcess(it->second, &code)) return -1;
  if (code != STILL_ACTIVE) {
    CloseHandle(it->second);
    g_children.erase(it);
  }
  return static_cast<int>(code);
}

int64_t ChildStartTime(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return 0;
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(it->second, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool KillChild(int pid, int64_t start_time) {
  if (pid <= 0 || start_time <= 0) return false;
  // ExitCode cannot close this handle and Spawn cannot replace this map entry
  // between checking its identity and termination. The handle, not the PID,
  // names the original Windows process throughout the operation.
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return false;
  HANDLE child = it->second;
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(child, &created, &exited, &kernel, &user)) return false;
  int64_t actual = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  if (actual != start_time || WaitForSingleObject(child, 0) != WAIT_TIMEOUT) return false;
  return TerminateProcess(child, 137) != FALSE;
}

bool Kill(int pid) {
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
    pipe.value = CreateFileW(name.c_str(), binder_wire::kPipeClientAccess, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                             SECURITY_IDENTIFICATION, nullptr);
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
  uint32_t hello[]{5 * sizeof(uint32_t), binder_wire::kHelloProc, binder_wire::kMagic,
                    binder_wire::kVersion, GetCurrentProcessId(), 10000};
  uint32_t length = 0;
  unsigned char reply[16]{};
  if (!transfer(hello, sizeof(hello), true) || !transfer(&length, sizeof(length), false) ||
      length != sizeof(reply) || !transfer(reply, sizeof(reply), false)) return false;
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
