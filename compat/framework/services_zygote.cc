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
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "services_zygote.h"
#include "app_container.h"
#include "../binder/launch_channel.h"
#include "../posix/include/gesher/binder_endpoint.h"
#include "../posix/include/gesher/cohost_bootstrap.h"
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

struct DeviceLifetime {
  uint64_t job = 0;
  uint64_t supervisor = 0;
  std::array<uint64_t, 3> stdio{};
};

using StandardHandles = std::array<peer::OwnedHandle, 3>;

struct StandardRelay {
  peer::OwnedHandle input;
  peer::OwnedHandle output;
};

DWORD WINAPI RelayStandardStream(void* data) {
  std::unique_ptr<StandardRelay> relay(static_cast<StandardRelay*>(data));
  char bytes[4096];
  DWORD size;
  while (ReadFile(relay->input.get(), bytes, sizeof(bytes), &size, nullptr) && size != 0) {
    DWORD offset = 0;
    while (offset < size) {
      DWORD written = 0;
      if (!WriteFile(relay->output.get(), bytes + offset, size - offset, &written, nullptr) ||
          written == 0) return 0;
      offset += written;
    }
  }
  return 0;
}

bool OpenScopedStandardHandles(StandardHandles* destination, StandardHandles* relays) {
  constexpr DWORD kStreams[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (size_t i = 0; i < destination->size(); ++i) {
    const HANDLE original = GetStdHandle(kStreams[i]);
    if (original == nullptr || original == INVALID_HANDLE_VALUE) {
      auto& result = (*destination)[i];
      result.reset(CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
      if (result.get() == INVALID_HANDLE_VALUE) return false;
      continue;
    }
    DWORD mode;
    HANDLE standard = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &standard,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    peer::OwnedHandle copy(standard);
    if (!GetConsoleMode(original, &mode)) {
      (*destination)[i] = std::move(copy);
      continue;
    }
    // Console handles cannot be used by a detached app. Relay the caller's
    // bytes within its own console and send only the appropriate pipe endpoint.
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 0)) return false;
    peer::OwnedHandle reader(read), writer(write);
    auto relay = std::make_unique<StandardRelay>();
    if (i == 0) {
      relay->input = std::move(copy);
      relay->output = std::move(writer);
      (*destination)[i] = std::move(reader);
    } else {
      relay->input = std::move(reader);
      relay->output = std::move(copy);
      (*destination)[i] = std::move(writer);
    }
    auto* context = relay.release();
    (*relays)[i].reset(CreateThread(nullptr, 0, RelayStandardStream, context, 0, nullptr));
    if (!(*relays)[i]) {
      delete context;
      return false;
    }
  }
  return true;
}

struct StandaloneInput {
  peer::OwnedHandle file;
  peer::OwnedHandle requests;
  peer::OwnedHandle child_requests;
  peer::OwnedHandle name;
};

DWORD PreparePinnedSource(uint64_t pin, StandaloneInput* source);
bool StandaloneSupervisor();
void WaitWithStandaloneInput(HANDLE process, StandaloneInput& code);

struct PinnedChildInput {
  peer::OwnedHandle process;
  StandaloneInput source;
};

struct CohostHandles {
  peer::OwnedHandle metadata;
  peer::OwnedHandle host;
  const CohostSpec* services = nullptr;
};

DWORD WINAPI ServePinnedChildInput(void* value) {
  std::unique_ptr<PinnedChildInput> input(static_cast<PinnedChildInput*>(value));
  WaitWithStandaloneInput(input->process.get(), input->source);
  return 0;
}

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

// Windows Binder supplies the exact Android UID. The decision lives in the
// authenticated host Java permission owner and is never sent by the guest.
class CapturePolicyServer {
 public:
  ~CapturePolicyServer() {
    if (stop_) SetEvent(stop_.get());
    if (thread_) WaitForSingleObject(thread_.get(), INFINITE);
  }

  bool Start(HANDLE authority, HANDLE broker,
             std::function<bool(const capture_policy::Request&)> allowed) {
    peer::OwnedHandle writer;
    if (!launch::PrivatePipe(&pipe_, &writer, true)) return false;
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ready_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_ || !ready_) return false;
    allowed_ = std::move(allowed);
    thread_.reset(CreateThread(nullptr, 0, Serve, this, 0, nullptr));
    if (!thread_) return false;
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), writer.get(), broker, &remote,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    launch::Request attach;
    attach.action = launch::Action::kAttachCaptureSessions;
    attach.process = launch::EncodeHandle(remote);
    return SubmitLaunchRequest(authority, attach);
  }

 private:
  bool Exchange(void* value, DWORD size, bool write) {
    ResetEvent(ready_.get());
    OVERLAPPED io{};
    io.hEvent = ready_.get();
    DWORD moved = 0;
    const BOOL done = write ? WriteFile(pipe_.get(), value, size, &moved, &io)
                            : ReadFile(pipe_.get(), value, size, &moved, &io);
    if (done) return moved == size;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE events[] = {stop_.get(), ready_.get()};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe_.get(), &io);
      GetOverlappedResult(pipe_.get(), &io, &moved, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe_.get(), &io, &moved, FALSE) && moved == size;
  }

  static DWORD WINAPI Serve(void* self) {
    auto& server = *static_cast<CapturePolicyServer*>(self);
    capture_policy::Request request{};
    while (server.Exchange(&request, sizeof(request), false)) {
      uint32_t allowed = request.uid >= 10000 && request.uid <= 19999 &&
          request.kind >= capture_policy::Kind::kMicrophone &&
          request.kind <= capture_policy::Kind::kCameraFinish &&
          request.attribution.valid() && server.allowed_(request);
      if (!server.Exchange(&allowed, sizeof(allowed), true)) break;
    }
    server.allowed_({0, capture_policy::Kind::kCameraReset});
    return 0;
  }

  peer::OwnedHandle pipe_, thread_, ready_, stop_;
  std::function<bool(const capture_policy::Request&)> allowed_;
};

std::mutex g_lock;
std::mutex g_bootstrap_lock;  // one publisher for the private control stream
StandardHandles g_scoped_relays;
std::map<int, HANDLE> g_children;  // processes this one started, for their exit codes
std::map<int, HANDLE> g_compiler_jobs;  // lifetime-bound compiler resource budgets
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

  DWORD Initialize(HANDLE* handles, size_t count, const SECURITY_CAPABILITIES* sandbox = nullptr,
                   HANDLE* lifetime = nullptr) {
    SIZE_T size = 0;
    DWORD attributes = 1 + (sandbox != nullptr) + (lifetime != nullptr);
    InitializeProcThreadAttributeList(nullptr, attributes, 0, &size);
    if (size == 0) return GetLastError();
    storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, attributes, 0, &size)) return GetLastError();
    value = list;
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles, count * sizeof(HANDLE), nullptr, nullptr)) {
      return GetLastError();
    }
    if (sandbox != nullptr &&
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   const_cast<SECURITY_CAPABILITIES*>(sandbox),
                                   sizeof(*sandbox), nullptr, nullptr)) return GetLastError();
    if (lifetime != nullptr &&
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, lifetime,
                                   sizeof(*lifetime), nullptr, nullptr)) return GetLastError();
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
  std::u16string local_app_data;
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
  std::wstring local_app_data;
  error = appcontainer::LocalAppDataPath(&local_app_data);
  if (error != ERROR_SUCCESS) return error;
  out->local_app_data = U(local_app_data.c_str(), local_app_data.size());
  return ERROR_SUCCESS;
}

std::vector<std::u16string> ContainerEnvironment(const Container& container) {
  // GetTempPathW inside an AppContainer derives the path from LOCALAPPDATA
  // even when TEMP and TMP are set. The host may isolate its device with a
  // different LOCALAPPDATA, but the OS still owns the real package profile.
  const auto temp = container.folder + u"\\Temp";
  return {u"TEMP=" + temp, u"TMP=" + temp, u"LOCALAPPDATA=" + container.local_app_data};
}

int SpawnImpl(const std::u16string& command_line, std::vector<std::u16string> set,
              const std::vector<std::u16string>& unset, const std::u16string& log_path,
              bool breakaway, const AppContainerSpec* sandbox,
              const BootstrapHandles* bootstrap = nullptr,
              const Enrollment* enrollment = nullptr, HANDLE lifetime_job = nullptr,
              std::unique_ptr<PinnedChildInput> pinned_input = nullptr,
              const StandardHandles* stdio = nullptr,
              uint32_t compiler_memory_mib = 0, uint32_t compiler_cpu_rate = 0) {
  const bool compiler = compiler_memory_mib != 0;
  if (compiler && (breakaway || sandbox != nullptr || enrollment != nullptr || compiler_memory_mib < 512 || compiler_memory_mib > 2048 ||
                   compiler_cpu_rate < 100 || compiler_cpu_rate > 10000))
    return -ERROR_INVALID_PARAMETER;
  OwnedHandle compiler_job;
  if (compiler) {
    compiler_job.value = CreateJobObjectW(nullptr, nullptr);
    if (compiler_job.value == nullptr) return -static_cast<int>(GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    // CREATE_PROCESS's BELOW_NORMAL flag needs no elevated base-priority
    // privilege; JOB_OBJECT_LIMIT_PRIORITY_CLASS would impose that requirement.
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(compiler_memory_mib) * 1024 * 1024;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu = {};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = compiler_cpu_rate;
    if (!SetInformationJobObject(compiler_job.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !SetInformationJobObject(compiler_job.value, JobObjectCpuRateControlInformation,
                                 &cpu, sizeof(cpu)))
      return -static_cast<int>(GetLastError());
  }
  Container container;
  if (sandbox != nullptr) {
    DWORD error = PrepareContainer(*sandbox, &container);
    if (error != ERROR_SUCCESS) return -static_cast<int>(error);
    auto temporary = ContainerEnvironment(container);
    set.insert(set.end(), temporary.begin(), temporary.end());
  }
  const DWORD attribute_count = 1 + (sandbox != nullptr) + (lifetime_job != nullptr);
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
  // Appending only: stdout and stderr share the handle, and every write goes
  // to the end, also with another process appending to the same log.
  HANDLE log = CreateFileW(W(log_path), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return -static_cast<int>(GetLastError());
  const DWORD null_access = bootstrap != nullptr && bootstrap->endpoint == launch::Endpoint::kApplication
      ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
  HANDLE nul = CreateFileW(L"NUL", null_access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, nullptr);
  if (nul == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    CloseHandle(log);
    return -static_cast<int>(error);
  }
  std::vector<HANDLE> handles{nul};
  if (stdio == nullptr) handles.push_back(log);
  peer::OwnedHandle inherited_channel;
  peer::OwnedHandle inherited_broker;
  peer::OwnedHandle inherited_job_delivery;
  StandardHandles inherited_stdio;
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
    if (enrollment != nullptr) record.uid = enrollment->uid;
    if (bootstrap->endpoint == launch::Endpoint::kApplication) {
      record.null_device = launch::EncodeHandle(nul);
      if (pinned_input) {
        auto& source = pinned_input->source;
        handles.insert(handles.end(), {source.file.get(), source.child_requests.get(),
                                       source.name.get()});
        record.code_file = launch::EncodeHandle(source.file.get());
        record.code_channel = launch::EncodeHandle(source.child_requests.get());
        record.code_name = launch::EncodeHandle(source.name.get());
      }
    }
    if (stdio != nullptr) {
      for (size_t i = 0; i < inherited_stdio.size(); ++i) {
        inherited_stdio[i] = make_inheritable((*stdio)[i].get());
        if (!inherited_stdio[i]) {
          CloseHandle(nul);
          CloseHandle(log);
          return -ERROR_INVALID_HANDLE;
        }
        handles.push_back(inherited_stdio[i].get());
      }
    }
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
    si.StartupInfo.hStdInput = stdio != nullptr ? inherited_stdio[0].get() : nul;
    si.StartupInfo.hStdOutput = stdio != nullptr ? inherited_stdio[1].get() : log;
    si.StartupInfo.hStdError = stdio != nullptr ? inherited_stdio[2].get() : log;
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
    if (compiler) flags |= CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
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
    MEMORY_PRIORITY_INFORMATION memory_priority = {MEMORY_PRIORITY_LOW};
    if (ok && compiler &&
        (!AssignProcessToJobObject(compiler_job.value, pi.hProcess) ||
         !SetProcessInformation(pi.hProcess, ProcessMemoryPriority, &memory_priority,
                                 sizeof(memory_priority)))) {
      DWORD error = GetLastError();
      TerminateProcess(pi.hProcess, 137);
      WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      result = -static_cast<int>(error);
      ok = FALSE;
    }
    if (ok && compiler && ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
      DWORD error = GetLastError();
      TerminateProcess(pi.hProcess, 137);
      WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      result = -static_cast<int>(error);
      ok = FALSE;
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
          if (compiler) {
            g_compiler_jobs[result] = compiler_job.value;
            compiler_job.value = nullptr;
          }
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
          if (retained && pinned_input) {
            HANDLE target = nullptr;
            retained = DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(),
                                       &target, SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, 0);
            if (retained) {
              pinned_input->process.reset(target);
              PinnedChildInput* input = pinned_input.release();
              peer::OwnedHandle worker(CreateThread(nullptr, 0, ServePinnedChildInput,
                                                     input, 0, nullptr));
              retained = static_cast<bool>(worker);
              if (!retained) pinned_input.reset(input);
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
    } else if (result == 0) {
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
          bool breakaway, uint32_t compiler_memory_mib, uint32_t compiler_cpu_rate) {
  return SpawnImpl(command_line, set, unset, log_path, breakaway, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr, compiler_memory_mib, compiler_cpu_rate);
}

int SpawnInAppContainer(const std::u16string& command_line,
                        const std::vector<std::u16string>& set,
                        const std::vector<std::u16string>& unset, const std::u16string& log_path,
                        const AppContainerSpec& sandbox) {
  return SpawnImpl(command_line, set, unset, log_path, false, &sandbox);
}

std::vector<int> StartProtectedDevice(const std::u16string& device,
    const std::vector<std::u16string>& commands, const std::vector<std::u16string>& logs,
    const std::vector<std::u16string>& environment, bool scoped) {
  std::lock_guard<std::mutex> bootstrap_lock(g_bootstrap_lock);
  process_identity::Process self;
  if (device.empty() || commands.size() != 3 || logs.size() != 3 ||
      !process_identity::ReadLiveProcess(GetCurrentProcess(), &self) || self.principal.restricted ||
      self.principal.container.size != 0 || self.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID) {
    return {};
  }
  StandardHandles stdio;
  if (scoped && !OpenScopedStandardHandles(&stdio, &g_scoped_relays)) return {};

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
  DeviceLifetime delivered;
  bool copied = false;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto child = g_children.find(system);
    if (child != g_children.end()) {
      auto to_child = [&](HANDLE local, DWORD rights, DWORD flags, uint64_t* remote) {
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), local, child->second, &copy, rights, FALSE,
                             flags)) return false;
        *remote = launch::EncodeHandle(copy);
        return true;
      };
      copied = to_child(lifetime_job.get(), 0, DUPLICATE_SAME_ACCESS, &delivered.job);
      if (scoped) {
        copied = copied && to_child(GetCurrentProcess(),
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, 0, &delivered.supervisor);
        for (size_t i = 0; copied && i < stdio.size(); ++i) {
          copied = to_child(stdio[i].get(), 0, DUPLICATE_SAME_ACCESS, &delivered.stdio[i]);
        }
      }
    }
  }
  if (!copied || !launch::Transfer(job_writer.get(), &delivered, sizeof(delivered), true)) {
    TerminateJobObject(lifetime_job.get(), ERROR_ACCESS_DENIED);
    return {};
  }
  return {broker, manager, system};
}

void FinishScopedStandardStreams() {
  StandardHandles relays;
  {
    std::lock_guard<std::mutex> lock(g_bootstrap_lock);
    relays = std::move(g_scoped_relays);
  }
  if (relays[0]) CancelSynchronousIo(relays[0].get());
  // The supervisor still owns its end of the real console. Drain both
  // output pipes once the temporary job's inherited writers have closed.
  for (size_t i = 1; i < relays.size(); ++i) {
    if (relays[i] && WaitForSingleObject(relays[i].get(), 5000) == WAIT_TIMEOUT) {
      CancelSynchronousIo(relays[i].get());
    }
  }
}

uint32_t CaptureCohostStandardStreams(std::array<void*, 3>* handles) {
  if (handles == nullptr || !StandaloneSupervisor()) return ERROR_ACCESS_DENIED;
  StandardHandles captured;
  {
    std::lock_guard<std::mutex> lock(g_bootstrap_lock);
    if (!OpenScopedStandardHandles(&captured, &g_scoped_relays)) {
      const DWORD failure = GetLastError();
      return failure == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : failure;
    }
  }
  for (size_t i = 0; i < captured.size(); ++i) (*handles)[i] = captured[i].release();
  return ERROR_SUCCESS;
}

namespace {

struct Writer {
  peer::OwnedHandle channel;
  peer::OwnedHandle broker;
  bool scoped = false;
  StandardHandles stdio;
  std::map<std::array<uint8_t, 16>, peer::OwnedHandle> installations;
  std::map<std::array<uint8_t, 16>, uint64_t> stopped_through;
};

void CALLBACK StopSupervisedDevice(PVOID job, BOOLEAN) {
  TerminateJobObject(static_cast<HANDLE>(job), ERROR_PROCESS_ABORTED);
}

Writer ReadWriter() {
  launch::Startup startup;
  process_identity::Process self;
  if (!launch::ReadStartup(launch::Endpoint::kSystemServer, &startup) ||
      !process_identity::ReadLiveProcess(GetCurrentProcess(), &self) || self.principal.restricted ||
      self.principal.container.size != 0 || self.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID) {
    return {};
  }
  Writer writer;
  writer.channel.reset(launch::DecodeHandle(startup.channel));
  writer.broker.reset(launch::DecodeHandle(startup.broker));
  peer::OwnedHandle job_pipe(launch::DecodeHandle(startup.job_delivery));
  if (!SetHandleInformation(writer.channel.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(writer.broker.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(job_pipe.get(), HANDLE_FLAG_INHERIT, 0)) return {};
  DeviceLifetime delivered;
  if (!launch::Transfer(job_pipe.get(), &delivered, sizeof(delivered), false)) return {};
  peer::OwnedHandle job(launch::DecodeHandle(delivered.job));
  peer::OwnedHandle supervisor(launch::DecodeHandle(delivered.supervisor));
  writer.scoped = delivered.supervisor != 0;
  for (size_t i = 0; i < writer.stdio.size(); ++i) {
    const bool has_streams = delivered.stdio[0] != 0;
    if ((delivered.stdio[i] != 0) != has_streams || (has_streams && !writer.scoped)) return {};
    writer.stdio[i].reset(launch::DecodeHandle(delivered.stdio[i]));
  }
  BOOL in_job = FALSE;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if (!job || !IsProcessInJob(GetCurrentProcess(), job.get(), &in_job) || !in_job ||
      !QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits), nullptr) ||
      (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0) return {};
  HANDLE broker_job = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), job.get(), writer.broker.get(), &broker_job,
                       JOB_OBJECT_QUERY, FALSE, 0)) return {};
  launch::Request attach;
  attach.action = launch::Action::kAttachJob;
  attach.process = launch::EncodeHandle(broker_job);
  if (!SubmitLaunchRequest(writer.channel.get(), attach)) return {};

  launch::Request enable_netlink;
  enable_netlink.action = launch::Action::kEnableNetlinkInspection;
  enable_netlink.pid = self.pid;
  enable_netlink.birth = self.birth;
  if (!SubmitLaunchRequest(writer.channel.get(), enable_netlink)) return {};
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_job != nullptr) return {};
    if (writer.scoped) {
      HANDLE watch;
      if (!supervisor || GetProcessId(supervisor.get()) == 0 ||
          GetProcessId(supervisor.get()) == GetCurrentProcessId() ||
          !RegisterWaitForSingleObject(&watch, supervisor.get(), StopSupervisedDevice,
                                       job.get(), INFINITE, WT_EXECUTEONLYONCE)) {
        TerminateJobObject(job.get(), ERROR_ACCESS_DENIED);
        return {};
      }
      // The wait and its process handle belong to this server's lifetime.
      // Keeping the process handle pins the exact supervisor across PID reuse.
      (void)supervisor.release();
    }
    g_job = job.release();
  }
  return writer;
}

Writer& Authority() {
  static Writer authority = ReadWriter();
  return authority;
}

bool StopInstallation(Writer& authority, const std::array<uint8_t, 16>& installation) {
  auto at = authority.installations.find(installation);
  if (at == authority.installations.end()) return true;
  HANDLE job = at->second.get();
  if (!TerminateJobObject(job, 137)) return false;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &usage,
                                   sizeof(usage), nullptr)) return false;
    if (usage.ActiveProcesses == 0) {
      authority.installations.erase(at);
      return true;
    }
    if (GetTickCount64() >= deadline) return false;
    Sleep(2);
  }
}

}  // namespace

bool RegisterProtectedCapturePolicy(
    std::function<bool(const capture_policy::Request&)> allowed) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& owner = Authority();
  if (!owner.channel || !allowed) return false;
  auto service = std::make_unique<CapturePolicyServer>();
  if (!service->Start(owner.channel.get(), owner.broker.get(), std::move(allowed))) return false;
  // This private policy endpoint lives with the protected SystemServer.
  (void)service.release();
  return true;
}

bool HasProtectedLaunchAuthority(bool scoped) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  return authority.channel && authority.broker && (!scoped || authority.scoped) &&
         WaitForSingleObject(authority.broker.get(), 0) == WAIT_TIMEOUT;
}

bool PrepareProtectedOperator(int broker_pid, uint64_t birth) {
  return broker_pid > 0 && binder_endpoint::Current().PrepareOperator(
      static_cast<DWORD>(broker_pid), birth);
}

int SpawnProtectedApp(const std::u16string& command_line, const std::vector<std::u16string>& set,
    const std::vector<std::u16string>& unset, const std::u16string& log_path,
    const AppContainerSpec& sandbox, uint32_t uid, const std::array<uint8_t, 16>& installation,
    uint64_t start_sequence, uint64_t code_pin) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  auto stopped = authority.stopped_through.find(installation);
  if (!authority.channel || !authority.broker || uid < 10000 || uid > 19999 ||
      start_sequence == 0 ||
      (stopped != authority.stopped_through.end() && start_sequence <= stopped->second) ||
      installation == std::array<uint8_t, 16>{}) return -ERROR_ACCESS_DENIED;
  HANDLE query_broker = nullptr;
  constexpr DWORD kPinRights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  if (!DuplicateHandle(GetCurrentProcess(), authority.broker.get(), GetCurrentProcess(),
                       &query_broker, kPinRights, FALSE, 0)) return -ERROR_ACCESS_DENIED;
  peer::OwnedHandle app_broker(query_broker);
  const BootstrapHandles app_handles{launch::Endpoint::kApplication, nullptr, app_broker.get()};
  auto job = authority.installations.find(installation);
  if (job == authority.installations.end()) {
    peer::OwnedHandle created(CreateLifetimeJob());
    if (!created) return -static_cast<int>(GetLastError());
    job = authority.installations.emplace(installation, std::move(created)).first;
  }
  bool channel_failed = false;
  const Enrollment enrollment{authority.channel.get(), authority.broker.get(),
                              peer::Role::kApplication, uid, installation, &channel_failed};
  std::unique_ptr<PinnedChildInput> input;
  if (code_pin != 0) {
    input = std::make_unique<PinnedChildInput>();
    const DWORD error = PreparePinnedSource(code_pin, &input->source);
    if (error != ERROR_SUCCESS) return -static_cast<int>(error);
  }
  const auto* stdio = input && authority.stdio[0] ? &authority.stdio : nullptr;
  int pid = SpawnImpl(command_line, set, unset, log_path, false, &sandbox, &app_handles,
                      &enrollment, job->second.get(), std::move(input), stdio);
  if (pid > 0 && stdio != nullptr) authority.stdio = {};
  if (channel_failed) authority = {};
  return pid;
}

bool StopProtectedInstallation(const std::array<uint8_t, 16>& installation,
                               uint64_t stop_through) {
  std::lock_guard<std::mutex> lock(g_bootstrap_lock);
  auto& authority = Authority();
  if (!authority.channel || !authority.broker || installation == std::array<uint8_t, 16>{}) {
    return false;
  }
  auto& previous = authority.stopped_through[installation];
  previous = (std::max)(previous, stop_through);
  return StopInstallation(authority, installation);
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
  if (accepted && revoke && !StopInstallation(authority, installation)) {
    // A broken sandbox lifetime cannot safely keep serving the old install.
    authority = {};
    return false;
  }
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

namespace {

struct PinnedProgram {
  peer::OwnedHandle file;
  std::wstring path;
};

std::mutex g_standalone_sources_lock;
std::map<uint64_t, PinnedProgram> g_standalone_sources;
uint64_t g_next_standalone_source = 1;

bool StandaloneSupervisor() {
  process_identity::Process self;
  return process_identity::ReadLiveProcess(GetCurrentProcess(), &self) &&
         self.principal.container.size == 0 && !self.principal.restricted &&
         self.principal.integrity >= SECURITY_MANDATORY_MEDIUM_RID;
}

DWORD RetainStandaloneSource(uint64_t pin, StandaloneInput* source, std::wstring* path) {
  std::lock_guard<std::mutex> lock(g_standalone_sources_lock);
  const auto found = g_standalone_sources.find(pin);
  if (found == g_standalone_sources.end()) return ERROR_INVALID_HANDLE;
  HANDLE inherited = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), found->second.file.get(), GetCurrentProcess(),
                       &inherited, 0, TRUE, DUPLICATE_SAME_ACCESS)) return GetLastError();
  source->file.reset(inherited);
  *path = found->second.path;
  return ERROR_SUCCESS;
}

DWORD PreparePinnedSource(uint64_t pin, StandaloneInput* source) {
  std::wstring path;
  const DWORD result = RetainStandaloneSource(pin, source, &path);
  if (result != ERROR_SUCCESS) return result;
  if (!launch::PrivatePipe(&source->requests, &source->child_requests)) return ERROR_ACCESS_DENIED;
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(source->requests.get(), &mode, nullptr, nullptr) ||
      !SetHandleInformation(source->child_requests.get(), HANDLE_FLAG_INHERIT,
                            HANDLE_FLAG_INHERIT)) return GetLastError();
  using Name = binder_bootstrap::StandaloneName;
  peer::OwnedHandle name(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                            0, sizeof(Name), nullptr));
  if (!name) return GetLastError();
  auto* record = static_cast<Name*>(MapViewOfFile(name.get(), FILE_MAP_WRITE, 0, 0, sizeof(Name)));
  if (record == nullptr) return GetLastError();
  record->length = static_cast<uint32_t>(path.size());
  std::memcpy(record->path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
  UnmapViewOfFile(record);
  HANDLE read_only = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), name.get(), GetCurrentProcess(),
                       &read_only, FILE_MAP_READ, TRUE, 0)) return GetLastError();
  source->name.reset(read_only);
  return ERROR_SUCCESS;
}

void WaitWithStandaloneInput(HANDLE process, StandaloneInput& code) {
  code.child_requests.reset();
  while (WaitForSingleObject(process, 8) == WAIT_TIMEOUT) {
    DWORD available = 0;
    if (!PeekNamedPipe(code.requests.get(), nullptr, 0, nullptr, &available, nullptr)) break;
    if (available < sizeof(uint32_t)) continue;
    uint32_t request = 0;
    const ULONGLONG deadline = GetTickCount64() + 200;
    if (!launch::Transfer(code.requests.get(), &request, sizeof(request), false, deadline) ||
        request != binder_bootstrap::kOpenStandaloneInput) break;
    peer::OwnedHandle fresh(ReOpenFile(code.file.get(), GENERIC_READ, FILE_SHARE_READ, 0));
    HANDLE remote = nullptr;
    if (fresh.get() != INVALID_HANDLE_VALUE &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      DuplicateHandle(GetCurrentProcess(), fresh.get(), process, &remote, 0, FALSE,
                      DUPLICATE_SAME_ACCESS);
    }
    uint64_t response = launch::EncodeHandle(remote);
    if (!launch::Transfer(code.requests.get(), &response, sizeof(response), true, deadline)) break;
  }
  code.requests.reset();
}

struct StandaloneRecordBroker {
  peer::OwnedHandle process;
  peer::OwnedHandle authority;

  ~StandaloneRecordBroker() {
    // The control pipe tells the broker to release its job query handle.
    // Closing it first also handles an early failure after the job was attached.
    authority.reset();
    if (process && WaitForSingleObject(process.get(), 500) == WAIT_TIMEOUT &&
        TerminateProcess(process.get(), ERROR_BROKEN_PIPE)) {
      WaitForSingleObject(process.get(), 500);
    }
  }

  DWORD Start(const std::u16string& runtime, HANDLE job, PSID container) {
    const size_t name = runtime.find_last_of(u"/\\");
    if (name == std::u16string::npos) return ERROR_PATH_NOT_FOUND;
    const std::u16string executable = runtime.substr(0, name + 1) + u"binderd.exe";
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(container, &sid)) return GetLastError();
    const auto command = u"\"" + executable + u"\" " + U(sid, wcslen(sid));
    LocalFree(sid);
    peer::OwnedHandle receiver;
    if (!launch::PrivatePipe(&receiver, &authority)) return ERROR_ACCESS_DENIED;
    const BootstrapHandles bootstrap{launch::Endpoint::kRecordBroker, receiver.get()};
    const int pid = SpawnImpl(command, {}, {u"GESHER_BINDER"}, u"NUL",
                              false, nullptr, &bootstrap, nullptr, job);
    receiver.reset();
    if (pid <= 0) return pid < 0 ? -pid : ERROR_ACCESS_DENIED;
    {
      std::lock_guard<std::mutex> lock(g_lock);
      const auto child = g_children.find(pid);
      if (child == g_children.end()) return ERROR_INVALID_HANDLE;
      process.reset(child->second);
      g_children.erase(child);
    }
    HANDLE broker_job = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), job, process.get(), &broker_job,
                         JOB_OBJECT_QUERY, FALSE, 0)) return GetLastError();
    launch::Request attach;
    attach.action = launch::Action::kAttachJob;
    attach.process = launch::EncodeHandle(broker_job);
    if (!SubmitLaunchRequest(authority.get(), attach)) return ERROR_ACCESS_DENIED;

    const auto pipe_name = binder_wire::kStandaloneRecordPipe + std::to_wstring(pid);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do {
      if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return ERROR_BROKEN_PIPE;
      peer::OwnedHandle pipe(CreateFileW(pipe_name.c_str(), binder_wire::kPipeClientAccess, 0,
          nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
      if (pipe.get() != INVALID_HANDLE_VALUE) {
        ULONG server = 0;
        return GetNamedPipeServerProcessId(pipe.get(), &server) && server == static_cast<DWORD>(pid)
                   ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
      }
      Sleep(10);
    } while (GetTickCount64() < deadline);
    return ERROR_TIMEOUT;
  }
};

uint32_t Supervise(std::u16string command, std::u16string environment, uint32_t* exit_code,
                  const Container* sandbox, uint32_t uid, StandaloneInput* code,
                  const CohostHandles* cohost = nullptr, const StandardHandles* streams = nullptr) {
  std::u16string exe = ExePath();
  if (exe.empty() || command.empty()) return ERROR_INVALID_PARAMETER;
  OwnedHandle input, output, error, device, job, process, thread, broker_pin;
  CapturePolicyServer capture_policy;
  StandaloneRecordBroker record_broker;
  const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  OwnedHandle* copies[] = {&input, &output, &error};
  HANDLE handles[10]{};
  for (size_t i = 0; i < 3; ++i) {
    HANDLE original = streams != nullptr ? (*streams)[i].get() : GetStdHandle(kinds[i]);
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
  job.value = CreateLifetimeJob();
  if (job.value == nullptr) return GetLastError();
  launch::Startup admission;
  size_t inherited = 3;
  if (sandbox != nullptr) {
    const DWORD failure = record_broker.Start(exe, job.value, sandbox->sid.value);
    if (failure != ERROR_SUCCESS) return failure;
    if (cohost && cohost->services->capture_allowed &&
        !capture_policy.Start(record_broker.authority.get(), record_broker.process.get(),
                              cohost->services->capture_allowed)) return ERROR_ACCESS_DENIED;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    device.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    if (device.value == INVALID_HANDLE_VALUE) return GetLastError();
    if (!DuplicateHandle(GetCurrentProcess(), record_broker.process.get(), GetCurrentProcess(),
                         &broker_pin.value, binder_endpoint::kPinRights, TRUE, 0)) return GetLastError();
    handles[inherited++] = device.value;
    handles[inherited++] = broker_pin.value;
    admission.endpoint = launch::Endpoint::kStandalone;
    admission.broker = launch::EncodeHandle(broker_pin.value);
    admission.null_device = launch::EncodeHandle(device.value);
    admission.uid = uid;
  }
  if (code != nullptr) {
    handles[inherited++] = code->file.get();
    handles[inherited++] = code->child_requests.get();
    handles[inherited++] = code->name.get();
    admission.code_file = launch::EncodeHandle(code->file.get());
    admission.code_channel = launch::EncodeHandle(code->child_requests.get());
    admission.code_name = launch::EncodeHandle(code->name.get());
  }
  if (cohost != nullptr) {
    handles[inherited++] = cohost->metadata.get();
    handles[inherited++] = cohost->host.get();
    admission.cohost_section = launch::EncodeHandle(cohost->metadata.get());
    admission.cohost_host = launch::EncodeHandle(cohost->host.get());
  }
  HandleList attributes;
  DWORD failure = attributes.Initialize(handles, inherited,
      sandbox != nullptr ? &sandbox->security : nullptr, sandbox != nullptr ? &job.value : nullptr);
  if (failure != ERROR_SUCCESS) return failure;

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = error.value;
  if (sandbox != nullptr) {
    startup.StartupInfo.lpReserved2 = reinterpret_cast<BYTE*>(&admission);
    startup.StartupInfo.cbReserved2 = sizeof(admission);
  }
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
  if (sandbox == nullptr && !AssignProcessToJobObject(job.value, process.value)) {
    failure = GetLastError();
    if (TerminateProcess(process.value, 1)) WaitForSingleObject(process.value, INFINITE);
    return failure;
  }
  if (sandbox != nullptr) {
    process_identity::Process admitted;
    BOOL member = FALSE;
    if (!process_identity::ReadLiveProcess(process.value, &admitted) ||
        !admitted.principal.container.Matches(sandbox->sid.value) ||
        admitted.principal.integrity >= SECURITY_MANDATORY_MEDIUM_RID ||
        !IsProcessInJob(process.value, job.value, &member) || !member) {
      return ERROR_ACCESS_DENIED;
    }
    Enrollment initial{record_broker.authority.get(), record_broker.process.get(),
        peer::Role::kApplication, uid};
    std::memcpy(initial.installation.data(), &admitted.birth, sizeof(admitted.birth));
    std::memcpy(initial.installation.data() + sizeof(admitted.birth), &admitted.pid, sizeof(admitted.pid));
    if (!EnrollProcess(initial, process.value, admitted, sandbox->sid.value)) return ERROR_ACCESS_DENIED;
  }
  if (cohost != nullptr) {
    failure = cohost->services->start_services(process.value, job.value);
    if (failure != ERROR_SUCCESS) {
      TerminateJobObject(job.value, failure);
      return failure;
    }
  }
  // Transfer only the foreground permission Windows actually gives this launcher.
  // Failure leaves the normal OS focus rules in force; it never fabricates focus.
  AllowSetForegroundWindow(child.dwProcessId);
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
    failure = GetLastError();
    if (cohost != nullptr) TerminateJobObject(job.value, failure);
    return failure;
  }
  if (code != nullptr) WaitWithStandaloneInput(process.value, *code);
  if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) return GetLastError();
  DWORD result = 0;
  if (!GetExitCodeProcess(process.value, &result)) return GetLastError();
  *exit_code = result;
  return ERROR_SUCCESS;
}

}  // namespace

uint32_t RestartWithUid(int uid, uint32_t* exit_code) {
  if (uid < 10000 || exit_code == nullptr) return ERROR_INVALID_PARAMETER;
  // Legacy non-contained standalone identity is checked again by the admission
  // code in the child. Production contained launches use the token below.
  const std::wstring decimal = std::to_wstring(uid);
  const std::u16string identity = U(decimal.c_str(), decimal.size());
  auto environment = EnvironmentBlock(
      {u"GESHER_UID=" + identity, u"GESHER_BINDER_UID=" + identity,
       u"GESHER_STANDALONE_RESTARTED=1"}, {});
  return Supervise(CommandLine(), std::move(environment), exit_code, nullptr, uid, nullptr);
}

uint32_t RunContainedStandalone(const std::u16string& command, const AppContainerSpec& spec,
                               const std::u16string& code, uint32_t uid, uint32_t* exit_code) {
  uint64_t pin = 0;
  if (!code.empty()) {
    const DWORD opened = PinStandaloneInput(code, &pin);
    if (opened != ERROR_SUCCESS) return opened;
  }
  const DWORD result = RunPinnedContainedStandalone(command, spec, pin, uid, exit_code);
  ReleaseStandaloneInput(pin);
  return result;
}

uint32_t PinStandaloneInput(const std::u16string& code, uint64_t* result) {
  if (code.empty() || result == nullptr || !StandaloneSupervisor()) return ERROR_ACCESS_DENIED;
  using Name = binder_bootstrap::StandaloneName;
  std::wstring path(Name::kMaxPath, L'\0');
  const DWORD length = GetFullPathNameW(W(code), Name::kMaxPath, path.data(), nullptr);
  if (length == 0 || length >= Name::kMaxPath) return ERROR_INVALID_NAME;
  path.resize(length);
  peer::OwnedHandle source(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (source.get() == INVALID_HANDLE_VALUE) return GetLastError();
  BY_HANDLE_FILE_INFORMATION file{};
  if (GetFileType(source.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(source.get(), &file) ||
      (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return ERROR_ACCESS_DENIED;
  std::lock_guard<std::mutex> lock(g_standalone_sources_lock);
  if (g_next_standalone_source == 0) return ERROR_TOO_MANY_OPEN_FILES;
  *result = g_next_standalone_source++;
  g_standalone_sources.emplace(*result, PinnedProgram{std::move(source), std::move(path)});
  return ERROR_SUCCESS;
}

void ReleaseStandaloneInput(uint64_t pin) {
  std::lock_guard<std::mutex> lock(g_standalone_sources_lock);
  g_standalone_sources.erase(pin);
}

uint32_t ReadStandaloneInput(uint64_t pin, int64_t offset, void* bytes,
                             uint32_t capacity, uint32_t* copied) {
  if (offset < 0 || bytes == nullptr || copied == nullptr || capacity == 0) {
    return ERROR_INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(g_standalone_sources_lock);
  const auto found = g_standalone_sources.find(pin);
  if (found == g_standalone_sources.end()) return ERROR_INVALID_HANDLE;
  HANDLE file = found->second.file.get();
  LARGE_INTEGER position{};
  position.QuadPart = offset;
  DWORD got = 0;
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ||
      !ReadFile(file, bytes, capacity, &got, nullptr)) return GetLastError();
  *copied = got;
  return ERROR_SUCCESS;
}

uint32_t RunPinnedContainedStandalone(const std::u16string& command, const AppContainerSpec& spec,
                                       uint64_t pin, uint32_t uid, uint32_t* exit_code) {
  if (uid < 10000 || uid > 19999 || exit_code == nullptr || !StandaloneSupervisor()) {
    return ERROR_ACCESS_DENIED;
  }
  Container sandbox;
  DWORD failure = PrepareContainer(spec, &sandbox);
  if (failure != ERROR_SUCCESS) return failure;
  StandaloneInput source;
  if (pin != 0) {
    failure = PreparePinnedSource(pin, &source);
    if (failure != ERROR_SUCCESS) return failure;
  }
  auto environment = EnvironmentBlock(ContainerEnvironment(sandbox),
      {u"GESHER_UID", u"GESHER_BINDER_UID", u"GESHER_BINDER", u"TMPDIR"});
  return Supervise(command, std::move(environment), exit_code, &sandbox, uid,
                   pin == 0 ? nullptr : &source);
}

namespace {
DWORD PrepareCohost(const CohostSpec& services, CohostHandles* handles) {
  if (!services.start_services || (services.storage_pipe.empty() &&
       (!services.credential.empty() || !services.device.empty())) ||
      (!services.storage_pipe.empty() &&
       (services.credential.empty() || services.device.empty())) ||
      (services.storage_pipe.empty() && services.permission_pipe.empty())) {
    return ERROR_INVALID_PARAMETER;
  }
  using Record = cohost_bootstrap::Metadata;
  peer::OwnedHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                0, sizeof(Record), nullptr));
  if (!mapping) return GetLastError();
  auto* bytes = MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(Record));
  if (bytes == nullptr) return GetLastError();
  auto* record = new (bytes) Record;
  const bool stored = cohost_bootstrap::Store(&record->credential, Wide(services.credential)) &&
      cohost_bootstrap::Store(&record->device, Wide(services.device)) &&
      cohost_bootstrap::Store(&record->credential_canonical, Wide(services.credential_canonical)) &&
      cohost_bootstrap::Store(&record->device_canonical, Wide(services.device_canonical)) &&
      cohost_bootstrap::Store(&record->credential_nt, Wide(services.credential_nt)) &&
      cohost_bootstrap::Store(&record->device_nt, Wide(services.device_nt)) &&
      cohost_bootstrap::Store(&record->build_prop, Wide(services.build_prop)) &&
      cohost_bootstrap::Store(&record->framework_res, Wide(services.framework_res)) &&
      cohost_bootstrap::Store(&record->storage_pipe, Wide(services.storage_pipe)) &&
      cohost_bootstrap::Store(&record->permission_pipe, Wide(services.permission_pipe));
  UnmapViewOfFile(bytes);
  if (!stored) return ERROR_FILENAME_EXCED_RANGE;
  HANDLE metadata = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), mapping.get(), GetCurrentProcess(),
                       &metadata, FILE_MAP_READ, TRUE, 0)) return GetLastError();
  handles->metadata.reset(metadata);
  HANDLE host = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                       &host, binder_endpoint::kPinRights, TRUE, 0)) return GetLastError();
  handles->host.reset(host);
  handles->services = &services;
  return ERROR_SUCCESS;
}
}  // namespace

uint32_t RunPinnedCohost(const std::u16string& command, const AppContainerSpec& spec,
                        uint64_t pin, uint32_t uid, const CohostSpec& services, uint32_t* exit_code) {
  if (pin == 0 || uid < 10000 || uid > 19999 || exit_code == nullptr ||
      !HasProtectedLaunchAuthority()) return ERROR_ACCESS_DENIED;
  Container sandbox;
  DWORD failure = PrepareContainer(spec, &sandbox);
  if (failure != ERROR_SUCCESS) return failure;
  StandaloneInput input;
  failure = PreparePinnedSource(pin, &input);
  if (failure != ERROR_SUCCESS) return failure;
  CohostHandles bootstrap;
  failure = PrepareCohost(services, &bootstrap);
  if (failure != ERROR_SUCCESS) return failure;
  StandardHandles streams;
  if (services.standard_handles[0] != nullptr) {
    for (size_t i = 0; i < streams.size(); ++i) {
      const HANDLE original = services.standard_handles[i];
      HANDLE copy = nullptr;
      if (original == nullptr || !DuplicateHandle(GetCurrentProcess(), original,
              GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return ERROR_INVALID_HANDLE;
      streams[i].reset(copy);
    }
  } else {
    std::lock_guard<std::mutex> lock(g_bootstrap_lock);
    streams = std::move(Authority().stdio);
  }
  auto environment = EnvironmentBlock(ContainerEnvironment(sandbox),
      {u"GESHER_BINDER_UID", u"GESHER_UID", u"TMPDIR"});
  return Supervise(command, std::move(environment), exit_code, &sandbox, uid, &input,
                   &bootstrap, streams[0] ? &streams : nullptr);
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
  if (WaitForSingleObject(it->second, 0) == WAIT_OBJECT_0) {
    CloseHandle(it->second);
    g_children.erase(it);
    auto job = g_compiler_jobs.find(pid);
    if (job != g_compiler_jobs.end()) {
      CloseHandle(job->second);
      g_compiler_jobs.erase(job);
    }
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

void* OpenProtectedChildFile(int pid, int64_t birth, const std::u16string& path) {
  if (pid <= 0 || birth <= 0 || path.empty()) return nullptr;
  peer::OwnedHandle child;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto found = g_children.find(pid);
    HANDLE retained = nullptr;
    if (found == g_children.end() ||
        !DuplicateHandle(GetCurrentProcess(), found->second, GetCurrentProcess(),
                         &retained, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) return nullptr;
    child.reset(retained);
  }
  process_identity::Process process;
  if (!process_identity::ReadLiveProcess(child.get(), &process) ||
      process.pid != static_cast<DWORD>(pid) || process.birth != static_cast<uint64_t>(birth) ||
      process.principal.container.size == 0) return nullptr;
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(child.get(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_token)) return nullptr;
  peer::OwnedHandle token(raw_token);
  HANDLE impersonation = nullptr;
  if (!DuplicateTokenEx(token.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
                        SecurityImpersonation, TokenImpersonation, &impersonation)) return nullptr;
  peer::OwnedHandle caller(impersonation);

  HANDLE previous_token = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &previous_token) &&
      GetLastError() != ERROR_NO_TOKEN) return nullptr;
  peer::OwnedHandle previous(previous_token);
  if (!SetThreadToken(nullptr, caller.get())) return nullptr;
  peer::OwnedHandle file(CreateFileW(W(path), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!SetThreadToken(nullptr, previous.get())) {
    TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
    return nullptr;
  }
  BY_HANDLE_FILE_INFORMATION attributes{};
  return file.get() != INVALID_HANDLE_VALUE && GetFileType(file.get()) == FILE_TYPE_DISK &&
      GetFileInformationByHandle(file.get(), &attributes) &&
      (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ? file.release() : nullptr;
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

uint64_t AvailableMemory() {
  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
}

int WaitChild(int pid, int64_t start_time, int timeout_ms, intptr_t cancel_event,
              bool stop_on_low_memory) {
  if (pid <= 0 || start_time <= 0 || timeout_ms < 0) return -1;
  OwnedHandle process, memory;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_children.find(pid);
    if (it == g_children.end()) return -1;
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(it->second, &created, &exited, &kernel, &user) ||
        ((static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) != start_time ||
        !DuplicateHandle(GetCurrentProcess(), it->second, GetCurrentProcess(), &process.value,
                          0, FALSE, DUPLICATE_SAME_ACCESS)) return -1;
  }
  HANDLE handles[3] = {process.value};
  DWORD count = 1;
  if (cancel_event != 0) handles[count++] = reinterpret_cast<HANDLE>(cancel_event);
  if (stop_on_low_memory) {
    memory.value = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (memory.value != nullptr) handles[count++] = memory.value;
  }
  DWORD wait = stop_on_low_memory && memory.value == nullptr ? WAIT_FAILED
      : WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(timeout_ms));
  int result = 0;
  if (wait != WAIT_OBJECT_0) {
    result = wait == WAIT_TIMEOUT ? -2
        : wait >= WAIT_OBJECT_0 + 1 && wait < WAIT_OBJECT_0 + count ? -3 : -1;
    // The pinned handle continues to name the original child even if a different
    // caller consumes the map's exit code or the numeric PID is later reused.
    TerminateProcess(process.value, 137);
    WaitForSingleObject(process.value, INFINITE);
  } else {
    DWORD code = 0;
    result = GetExitCodeProcess(process.value, &code) ? static_cast<int>(code) : -1;
  }
  {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_children.find(pid);
    FILETIME created, exited, kernel, user;
    if (it != g_children.end() && GetProcessTimes(it->second, &created, &exited, &kernel, &user) &&
        ((static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) == start_time) {
      CloseHandle(it->second);
      g_children.erase(it);
      auto job = g_compiler_jobs.find(pid);
      if (job != g_compiler_jobs.end()) {
        CloseHandle(job->second);
        g_compiler_jobs.erase(job);
      }
    }
  }
  return result;
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
