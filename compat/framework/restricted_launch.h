// Explicit restricted Windows guest launch primitive. Trusted host processes
// remain outside this boundary. No unrestricted fallback or implicit ACL grants.
#pragma once
#include <windows.h>
#include <vector>
#include <mutex>

namespace gesher::guest {

// Owns the exact child instance, primary thread and private kill-on-close job.
// Caller supplies an already provisioned AppContainer SID, executable/resources
// accessible to it, and only intentional inheritable resource handles. Profiles
// and resource ACLs remain caller owned; no network capabilities are granted.
// Publication/authority checks must happen before Resume(). This object must
// remain alive for the entire child lifetime. Stop() proves bounded retirement.
class RestrictedLaunch {
 public:
  struct [[nodiscard]] Retirement {
    bool confirmed = true;
    DWORD terminate_error = ERROR_SUCCESS;
    DWORD wait_error = ERROR_SUCCESS;
  };
  struct [[nodiscard]] Result {
    DWORD error;
    Retirement cleanup;
    Result(DWORD value) : error(value) {}
    Result(DWORD value, Retirement retired) : error(value), cleanup(retired) {}
  };
  RestrictedLaunch() = default;
  RestrictedLaunch(const RestrictedLaunch&) = delete;
  RestrictedLaunch& operator=(const RestrictedLaunch&) = delete;
  ~RestrictedLaunch() {
    Retirement retired = Stop();
    if (!retired.confirmed) {
      // Keep all owned handles available for explicit retry. Never silently
      // classify an unproved rollback as an ordinary rejected launch.
      std::lock_guard<std::mutex> lock(QuarantineLock());
      Quarantine().push_back({process_, thread_, job_, retired});
      OutputDebugStringW(L"Gesher restricted launch: retirement unconfirmed; owner quarantined\n");
      return;
    }
    if (thread_) CloseHandle(thread_);
    if (job_) CloseHandle(job_);  // kill-on-close even after failed termination
    if (process_) CloseHandle(process_);
  }

  // The command buffer is writable as required by CreateProcessW. Startup may
  // carry stdio and reserved launch metadata; its attribute list is replaced.
  // Only one attempt per object, including failed attempts. Errors are Win32.
  Result Start(const wchar_t* executable, wchar_t* command, void* environment,
              const wchar_t* directory, const STARTUPINFOW& startup, DWORD flags,
              PSID sid, const std::vector<HANDLE>& inherited) {
    if (attempted_) return ERROR_INVALID_STATE;
    attempted_ = true;
    const DWORD allowed = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                          CREATE_UNICODE_ENVIRONMENT;
    if (!executable || !*executable || !command || !sid || !IsValidSid(sid) ||
        inherited.empty() || (flags & ~allowed)) return ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < inherited.size(); ++i) {
      DWORD info = 0;
      if (!inherited[i] || inherited[i] == INVALID_HANDLE_VALUE ||
          !GetHandleInformation(inherited[i], &info) || !(info & HANDLE_FLAG_INHERIT))
        return ERROR_INVALID_HANDLE;
      for (size_t j = 0; j < i; ++j)
        if (inherited[i] == inherited[j]) return ERROR_INVALID_PARAMETER;
    }
    if (startup.dwFlags & STARTF_USESTDHANDLES) {
      for (HANDLE h : {startup.hStdInput, startup.hStdOutput, startup.hStdError}) {
        bool found = false;
        for (HANDLE allowed_handle : inherited) found |= allowed_handle == h;
        if (!found) return ERROR_INVALID_HANDLE;
      }
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) return GetLastError();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                             JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) return GetLastError();
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 3, 0, &bytes);
    if (!bytes) return GetLastError();
    std::vector<BYTE> storage(bytes);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 3, 0, &bytes)) return GetLastError();
    SECURITY_CAPABILITIES caps{};
    caps.AppContainerSid = sid;
    DWORD error = ERROR_SUCCESS;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                  &caps, sizeof(caps), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  const_cast<HANDLE*>(inherited.data()),
                                  inherited.size() * sizeof(HANDLE), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                  &job_, sizeof(job_), nullptr, nullptr)) {
      error = GetLastError();
    } else {
      STARTUPINFOEXW si{};
      si.StartupInfo = startup;
      si.StartupInfo.cb = sizeof(si);
      si.lpAttributeList = attrs;
      PROCESS_INFORMATION pi{};
      if (!CreateProcessW(executable, command, nullptr, nullptr, TRUE,
                          flags | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                          environment, directory, &si.StartupInfo, &pi)) {
        error = GetLastError();
      } else {
        process_ = pi.hProcess;
        thread_ = pi.hThread;
        pid_ = pi.dwProcessId;
        tid_ = pi.dwThreadId;
        BOOL in_job = FALSE;
        if (!IsProcessInJob(process_, job_, &in_job) || !in_job || !TokenMatches(sid))
          error = ERROR_ACCESS_DENIED;
        else ready_ = true;
      }
    }
    DeleteProcThreadAttributeList(attrs);
    if (error != ERROR_SUCCESS) return {error, Stop()};
    return error;
  }

  Result Resume() {
    if (!ready_ || resumed_) return ERROR_INVALID_STATE;
    // Exactly one suspended primary-thread count is expected. Never report
    // successful launch when guest execution is still suspended.
    DWORD previous = resume_(thread_);
    if (previous != 1) {
      DWORD error = previous == static_cast<DWORD>(-1) ? GetLastError() : ERROR_INVALID_STATE;
      ready_ = false;
      return {error, Stop()};
    }
    resumed_ = true;
    return ERROR_SUCCESS;
  }
  Retirement Stop() {
    ready_ = false;
    return Retire(process_, job_, terminate_, wait_);
  }
  // An owner destroyed after unconfirmed cleanup remains tracked. A trusted
  // supervisor can retry all such exact instances and inspect pending evidence.
  static std::vector<Retirement> PendingRetirements() {
    std::lock_guard<std::mutex> lock(QuarantineLock());
    std::vector<Retirement> result;
    for (const auto& pending : Quarantine()) result.push_back(pending.evidence);
    return result;
  }
  static size_t RetryQuarantined() {
    std::lock_guard<std::mutex> lock(QuarantineLock());
    auto& pending = Quarantine();
    for (auto it = pending.begin(); it != pending.end();) {
      it->evidence = Retire(it->process, it->job, TerminateJobObject, WaitForSingleObject);
      if (!it->evidence.confirmed) { ++it; continue; }
      CloseHandle(it->thread); CloseHandle(it->job); CloseHandle(it->process);
      it = pending.erase(it);
    }
    return pending.size();
  }
  HANDLE process() const { return process_; }
  HANDLE job() const { return job_; }
  DWORD pid() const { return pid_; }
  DWORD tid() const { return tid_; }

 private:
  friend struct RestrictedLaunchTestAccess;  // fixture-only private fault injection
  using Terminate = BOOL (WINAPI*)(HANDLE, UINT);
  using Wait = DWORD (WINAPI*)(HANDLE, DWORD);
  struct Pending { HANDLE process, thread, job; Retirement evidence; };
  static std::vector<Pending>& Quarantine() { static std::vector<Pending> pending; return pending; }
  static std::mutex& QuarantineLock() { static std::mutex lock; return lock; }
  static Retirement Retire(HANDLE process, HANDLE job, Terminate terminate, Wait wait) {
    Retirement result;
    if (!process) return result;
    if (wait(process, 0) != WAIT_OBJECT_0) {
      if (!terminate(job, 137)) result.terminate_error = GetLastError();
    }
    DWORD state = wait(process, 5000);
    result.confirmed = state == WAIT_OBJECT_0;
    if (!result.confirmed) result.wait_error = state == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    return result;
  }
  DWORD (WINAPI*resume_)(HANDLE) = ResumeThread;
  Terminate terminate_ = TerminateJobObject;
  Wait wait_ = WaitForSingleObject;
  bool TokenMatches(PSID expected) const {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process_, TOKEN_QUERY, &token)) return false;
    DWORD size = 0, isolated = 0;
    bool ok = GetTokenInformation(token, TokenIsAppContainer, &isolated, sizeof(isolated), &size) && isolated == 1;
    for (TOKEN_INFORMATION_CLASS kind : {TokenAppContainerSid, TokenCapabilities, TokenIntegrityLevel}) {
      size = 0;
      GetTokenInformation(token, kind, nullptr, 0, &size);
      std::vector<BYTE> data(size);
      if (!size || !GetTokenInformation(token, kind, data.data(), size, &size)) { ok = false; break; }
      if (kind == TokenAppContainerSid)
        ok &= EqualSid(expected, reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(data.data())->TokenAppContainer) != FALSE;
      else if (kind == TokenCapabilities)
        ok &= reinterpret_cast<TOKEN_GROUPS*>(data.data())->GroupCount == 0;
      else {
        PSID integrity = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data())->Label.Sid;
        ok &= IsValidSid(integrity) && *GetSidSubAuthorityCount(integrity) > 0 &&
            *GetSidSubAuthority(integrity, *GetSidSubAuthorityCount(integrity) - 1) == SECURITY_MANDATORY_LOW_RID;
      }
    }
    CloseHandle(token);
    return ok;
  }
  HANDLE process_ = nullptr, thread_ = nullptr, job_ = nullptr;
  DWORD pid_ = 0, tid_ = 0;
  bool attempted_ = false, ready_ = false, resumed_ = false;
};
}  // namespace gesher::guest
