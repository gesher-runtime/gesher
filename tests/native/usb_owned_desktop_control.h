// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
// Included only by the disposable protected-launch fixture, after its helpers.
// No production launcher or existing window-station/desktop policy is changed.
namespace owned_desktop_control {
constexpr wchar_t kReportName[] = L"owned-desktop-control.bin";
constexpr DWORD kMagic = 0x474f4443, kVersion = 2;
constexpr DWORD kStationAccess = STANDARD_RIGHTS_REQUIRED | 0x037f;
constexpr DWORD kDesktopAccess = STANDARD_RIGHTS_REQUIRED | 0x01ff;
constexpr DWORD kStationMinimum = 0x0327, kDesktopMinimum = 0x00c7;
struct Variant { const wchar_t* leaf; const char* name; bool current_station; DWORD station, desktop; };
constexpr Variant kVariants[]{
  {kReportName, "original", false, kStationAccess, kDesktopAccess},
  {L"owned-matrix-current-min.bin", "current-station/minimum-desktop", true, kStationMinimum, kDesktopMinimum},
  {L"owned-matrix-current-read.bin", "current-station/read-control-desktop", true, kStationMinimum, kDesktopMinimum | READ_CONTROL},
  {L"owned-matrix-current-full.bin", "current-station/full-desktop", true, kStationMinimum, kDesktopAccess},
  {L"owned-matrix-fresh-min.bin", "owned-minimum-station/minimum-desktop", false, kStationMinimum, kDesktopMinimum},
  {L"owned-matrix-fresh-station.bin", "owned-read-control-station/minimum-desktop", false, kStationMinimum | READ_CONTROL, kDesktopMinimum},
  {L"owned-matrix-fresh-desktop.bin", "owned-minimum-station/read-control-desktop", false, kStationMinimum, kDesktopMinimum | READ_CONTROL},
  {L"owned-matrix-fresh-both.bin", "owned-read-control-station/read-control-desktop", false, kStationMinimum | READ_CONTROL, kDesktopMinimum | READ_CONTROL},
};
struct Request {
  DWORD magic = kMagic, version = kVersion, size = sizeof(Request), session = 0;
  DWORD variant = 0;
  ULONGLONG deadline = 0;
  ULONG_PTR private_token = 0, lowbox_token = 0, job = 0, report = 0;
  DWORD private_bytes = 0, package_bytes = 0;
  alignas(DWORD) BYTE private_sid[SECURITY_MAX_SID_SIZE]{}, package_sid[SECURITY_MAX_SID_SIZE]{};
};
struct Row { DWORD create = ERROR_NOT_READY, resume = ERROR_NOT_READY, wait = ERROR_NOT_READY,
  exit = ERROR_NOT_READY, stop = ERROR_NOT_READY; };
struct Report {
  DWORD magic = kMagic, version = kVersion, size = sizeof(Report), setup = ERROR_NOT_READY;
  DWORD station_created = 0, desktop_created = 0, security_verified = 0, session = 0;
  DWORD variant = 0;
  DWORD station_access[2]{}, desktop_access[2]{};
  Row rows[4]{}; // private/default, lowbox/default, private/owned, lowbox/owned
  DWORD restore = ERROR_NOT_READY, desktop_close = ERROR_NOT_READY, station_close = ERROR_NOT_READY;
};
static_assert(sizeof(Request) <= 256 && sizeof(Report) <= 256 && std::is_trivially_copyable_v<Request>);
bool SystemCaller() {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
  const Handle token(raw);
  const auto sid = User(token.get());
  return IsWellKnownSid(const_cast<BYTE*>(sid.data()), WinLocalSystemSid) != FALSE;
}
DWORD Remaining(ULONGLONG deadline, DWORD maximum) {
  const ULONGLONG now = GetTickCount64();
  return now < deadline ? static_cast<DWORD>(std::min<ULONGLONG>(maximum, deadline - now)) : 0;
}
void CheckToken(HANDLE token, const Request& request, bool lowbox) {
  DWORD bytes = 0, session = 0, contained = 0; TOKEN_TYPE type{};
  Require(token && GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes) &&
      session == request.session && GetTokenInformation(token, TokenIsAppContainer, &contained, sizeof(contained), &bytes) &&
      contained == (lowbox ? 1ul : 0ul) && GetTokenInformation(token, TokenType, &type, sizeof(type), &bytes) &&
      type == TokenPrimary, "owned-desktop-token-shape", ERROR_ACCESS_DENIED);
  const auto user = User(token);
  Require(EqualSid(const_cast<BYTE*>(user.data()), const_cast<BYTE*>(request.private_sid)),
      "owned-desktop-token-user", ERROR_ACCESS_DENIED);
  if (lowbox) {
    alignas(TOKEN_APPCONTAINER_INFORMATION) BYTE data[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
    Require(GetTokenInformation(token, TokenAppContainerSid, data, sizeof(data), &bytes) &&
        EqualSid(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(data)->TokenAppContainer,
            const_cast<BYTE*>(request.package_sid)), "owned-desktop-token-package", ERROR_ACCESS_DENIED);
  }
}
void VerifyObject(HANDLE object, const Request& request, DWORD mask, DWORD all,
                  DWORD* effective = nullptr, bool owned = true) {
  DWORD size = 0;
  SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
  if (effective) information |= OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;
  GetUserObjectSecurity(object, &information, nullptr, 0, &size);
  Require(size && size <= 65536, "owned-desktop-object-security-size", ERROR_INVALID_DATA);
  std::vector<BYTE> data(size);
  Require(GetUserObjectSecurity(object, &information, data.data(), size, &size),
      "owned-desktop-object-security");
  PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  Require(GetSecurityDescriptorDacl(data.data(), &present, &acl, &defaulted) && present && acl && IsValidAcl(acl),
      "owned-desktop-object-dacl", ERROR_INVALID_ACL);
  if (owned) for (PSID subject : {const_cast<BYTE*>(request.private_sid), const_cast<BYTE*>(request.package_sid)}) {
    bool found = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
      void* raw = nullptr; Require(GetAce(acl, i, &raw), "owned-desktop-object-ace");
      const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
      if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(const_cast<DWORD*>(&ace->SidStart), subject) &&
          ace->Mask == mask) found = true;
    }
    Require(found, "owned-desktop-object-full-access", ERROR_ACCESS_DENIED);
  }
  PACL label = nullptr;
  Require(GetSecurityDescriptorSacl(data.data(), &present, &label, &defaulted) && present && label && IsValidAcl(label),
      "owned-desktop-object-label", ERROR_INVALID_ACL);
  bool low = false;
  for (DWORD i = 0; i < label->AceCount; ++i) {
    void* raw = nullptr; Require(GetAce(label, i, &raw), "owned-desktop-label-ace");
    const auto* ace = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(raw);
    if (ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE &&
        IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLowLabelSid) &&
        ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) low = true;
  }
  Require(low, "owned-desktop-object-low-label", ERROR_ACCESS_DENIED);
  if (!effective) return;
  const ULONG_PTR tokens[]{request.private_token, request.lowbox_token};
  for (unsigned i = 0; i < std::size(tokens); ++i) {
    HANDLE raw = nullptr;
    Require(DuplicateTokenEx(reinterpret_cast<HANDLE>(tokens[i]), TOKEN_QUERY, nullptr,
        SecurityImpersonation, TokenImpersonation, &raw), "owned-desktop-check-token");
    const Handle token(raw);
    for (DWORD bits = all; bits; bits &= bits - 1) {
      const DWORD bit = bits & (~bits + 1u);
      effective[i] |= SessionAllowed(data.data(), token.get(), bit);
    }
    Require(effective[i] == mask, "owned-desktop-effective-access", ERROR_ACCESS_DENIED);
  }
}
Row Probe(HANDLE token, const wchar_t* desktop, const Request& request, HANDLE child_job, bool selftest) {
  Row result; Handle process, thread;
  try {
    Require(Remaining(request.deadline, 1) != 0, "owned-desktop-probe-deadline", ERROR_TIMEOUT);
    const std::wstring image = Image().substr(0, Image().find_last_of(L'\\') + 1) + kGuestLoaderProbes[2];
    std::wstring command = Quote(image);
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;0x001fffff;;;SY)",
        SDDL_REVISION_1, &raw_descriptor, nullptr), "owned-desktop-process-object-policy");
    const Local descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(desktop);
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> memory(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    Require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "owned-desktop-probe-attributes");
    const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST p) { DeleteProcThreadAttributeList(p); };
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(release)> own(attributes, release);
    DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
    Require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
        &mitigation, sizeof(mitigation), nullptr, nullptr), "owned-desktop-probe-mitigation");
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    const BOOL created = selftest ? CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
        flags, nullptr, nullptr, &startup.StartupInfo, &child) :
        CreateProcessAsUserW(token, image.c_str(), command.data(), &security, &security, FALSE,
            flags, nullptr, nullptr, &startup.StartupInfo, &child);
    result.create = created ? ERROR_SUCCESS : GetLastError();
    if (!created) return result;
    process.reset(child.hProcess); thread.reset(child.hThread);
    Require(AssignProcessToJobObject(child_job, process.get()), "owned-desktop-child-atomic-before-resume");
    BOOL assigned = FALSE;
    Require(IsProcessInJob(process.get(), child_job, &assigned) && assigned,
        "owned-desktop-child-job", ERROR_ACCESS_DENIED);
    if (!selftest) Require(IsProcessInJob(process.get(), reinterpret_cast<HANDLE>(request.job), &assigned) && assigned,
        "owned-desktop-child-original-job", ERROR_ACCESS_DENIED);
    result.resume = ResumeThread(thread.get()) == 1 ? ERROR_SUCCESS : ERROR_INVALID_STATE;
    if (!result.resume) {
      result.wait = WaitForSingleObject(process.get(), Remaining(request.deadline, 750));
      if (result.wait == WAIT_OBJECT_0 && !GetExitCodeProcess(process.get(), &result.exit)) result.exit = GetLastError();
    }
  } catch (const Failure& failure) { if (!process) result.create = failure.status; else result.resume = failure.status; }
    catch (...) { if (!process) result.create = ERROR_NOT_ENOUGH_MEMORY; else result.resume = ERROR_NOT_ENOUGH_MEMORY; }
  if (process) {
    if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) TerminateProcess(process.get(), ERROR_TIMEOUT);
    result.stop = WaitForSingleObject(process.get(), Remaining(request.deadline, 250)) == WAIT_OBJECT_0 ? ERROR_SUCCESS : ERROR_TIMEOUT;
    if (result.stop) {
      TerminateJobObject(child_job, ERROR_TIMEOUT);
      Require(WaitForSingleObject(process.get(), Remaining(request.deadline, 250)) == WAIT_OBJECT_0,
          "owned-desktop-child-confirm-death", ERROR_TIMEOUT);
      result.stop = ERROR_SUCCESS;
    }
  }
  return result;
}
Report Execute(const Request& request, bool selftest = false) {
  Report report; report.session = request.session; report.variant = request.variant;
  const Variant& variant = kVariants[request.variant];
  HWINSTA station = nullptr;
  HDESK desktop = nullptr;
  const HWINSTA previous = GetProcessWindowStation();
  const HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
  const Handle job(CreateJobObjectW(nullptr, nullptr));
  bool all_children_dead = true;
  try {
    Require(job && previous && previous_desktop, "owned-desktop-initial-objects");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)), "owned-desktop-kill-job");
    GUID unique{}; wchar_t suffix[40]{};
    Require(SUCCEEDED(CoCreateGuid(&unique)) && StringFromGUID2(unique, suffix, static_cast<int>(std::size(suffix))) > 0,
        "owned-desktop-guid");
    std::wstring name = L"Gesher.OwnedProbe." + std::wstring(suffix);
    const std::wstring base_policy = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)" +
        (selftest ? L"(A;;GA;;;" + SidText(const_cast<BYTE*>(request.private_sid)) + L")" : L"") + L"S:(ML;;NW;;;LW)";
    PSECURITY_DESCRIPTOR raw = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(base_policy.c_str(), SDDL_REVISION_1, &raw, nullptr),
        "owned-desktop-initial-security");
    const Local initial(raw);
    SECURITY_ATTRIBUTES security{sizeof(security), initial.get(), FALSE};
    // Ordinary selftests cannot create named stations. Their positive result
    // covers only a fresh desktop under the unchanged current station.
    std::wstring desktop_name = L"Probe";
    if (selftest || variant.current_station) {
      wchar_t actual[512]{}; DWORD needed = 0;
      Require(GetUserObjectInformationW(previous, UOI_NAME, actual, sizeof(actual), &needed) &&
          needed && needed <= sizeof(actual) && std::wmemchr(actual, 0, std::size(actual)) &&
          !std::wcschr(actual, L'\\'), "owned-desktop-current-station-name");
      name = actual;
      desktop_name = L"Gesher.DesktopProbe." + std::wstring(suffix);
      if (!selftest) {
        Require(_wcsicmp(actual, L"WinSta0") == 0, "owned-desktop-matrix-interactive-station", ERROR_ACCESS_DENIED);
        VerifyObject(previous, request, variant.station, kStationAccess, report.station_access, false);
      }
    } else {
      station = CreateWindowStationW(name.c_str(), CWF_CREATE_ONLY, kStationAccess, &security);
      Require(station != nullptr, "owned-desktop-create-exclusive-station"); report.station_created = 1;
      Require(SetProcessWindowStation(station), "owned-desktop-set-process-station");
    }
    SetLastError(ERROR_SUCCESS);
    desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, kDesktopAccess, &security);
    const DWORD created_status = GetLastError();
    Require(desktop != nullptr && created_status != ERROR_ALREADY_EXISTS,
        "owned-desktop-create-exclusive-desktop", created_status);
    report.desktop_created = 1;
    Require((!station || SetProcessWindowStation(previous)) && SetThreadDesktop(previous_desktop),
        "owned-desktop-restore-before-launch");
    const auto grant = [&](HANDLE object, DWORD mask) {
      wchar_t access[16]{};
      Require(std::swprintf(access, std::size(access), L"0x%08lx", mask) > 0, "owned-desktop-access-text");
      const std::wstring ace = L"(A;;" + std::wstring(access) + L";;;";
      const std::wstring full_policy = L"D:P" + ace + L"SY)" + ace + L"BA)" +
          ace + SidText(const_cast<BYTE*>(request.private_sid)) + L")" +
          ace + SidText(const_cast<BYTE*>(request.package_sid)) + L")S:(ML;;NW;;;LW)";
      PSECURITY_DESCRIPTOR raw_full = nullptr;
      Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(full_policy.c_str(), SDDL_REVISION_1, &raw_full, nullptr),
          "owned-desktop-full-security");
      const Local full(raw_full);
      SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
      Require(SetUserObjectSecurity(object, &info, full.get()), "owned-desktop-set-owned-security");
    };
    if (station) {
      grant(station, variant.station);
      VerifyObject(station, request, variant.station, kStationAccess, request.variant ? report.station_access : nullptr);
    }
    grant(desktop, variant.desktop);
    VerifyObject(desktop, request, variant.desktop, kDesktopAccess, request.variant ? report.desktop_access : nullptr);
    report.security_verified = 1;
    const std::wstring destination = name + L"\\" + desktop_name;
    for (unsigned i = request.variant ? 2u : 0u; i < 4; ++i) {
      if (selftest && (i & 1)) continue;
      report.rows[i] = Probe(reinterpret_cast<HANDLE>((i & 1) ? request.lowbox_token : request.private_token),
          i < 2 ? L"winsta0\\default" : destination.c_str(), request, job.get(), selftest);
    }
    report.setup = ERROR_SUCCESS;
  } catch (const Failure& failure) { report.setup = failure.status; }
    catch (...) { report.setup = ERROR_NOT_ENOUGH_MEMORY; }
  if (job) {
    TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    all_children_dead = QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
        accounting.ActiveProcesses == 0;
  }
  report.restore = (!station || SetProcessWindowStation(previous)) && SetThreadDesktop(previous_desktop)
      ? ERROR_SUCCESS : GetLastError();
  // A failed death check is unavailable diagnostic evidence. Retain these
  // handles until helper process exit rather than claim pre-death cleanup.
  if (all_children_dead) {
    report.desktop_close = !desktop || CloseDesktop(desktop) ? ERROR_SUCCESS : GetLastError();
    report.station_close = !station || CloseWindowStation(station) ? ERROR_SUCCESS : GetLastError();
  } else report.desktop_close = report.station_close = ERROR_BUSY;
  return report;
}
DWORD Helper(const wchar_t* name, const wchar_t* pid_argument, const wchar_t* birth_argument,
             const wchar_t* deadline_argument) {
  // Check the real caller before parsing or using any supplied endpoint.
  if (!SystemCaller()) return ERROR_ACCESS_DENIED;
  wchar_t* end = nullptr;
  const unsigned long long pid = std::wcstoull(pid_argument, &end, 10);
  if (!pid || pid > MAXDWORD || !end || *end) return ERROR_INVALID_PARAMETER;
  const unsigned long long birth = std::wcstoull(birth_argument, &end, 16);
  if (!birth || !end || *end) return ERROR_INVALID_PARAMETER;
  const unsigned long long deadline = std::wcstoull(deadline_argument, &end, 10);
  const auto now = GetTickCount64();
  if (!end || *end || deadline <= now || deadline - now > 5000 ||
      std::wcslen(name) > 200 || !std::wstring_view(name).starts_with(L"\\\\.\\pipe\\Gesher.OwnedDesktop.Control."))
    return ERROR_INVALID_PARAMETER;
  const Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!parent || !GetProcessTimes(parent.get(), &created, &exited, &kernel, &user) ||
      ((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime) != birth ||
      WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(parent.get(), TOKEN_QUERY, &raw_token)) return ERROR_ACCESS_DENIED;
  const Handle parent_token(raw_token);
  const auto parent_sid = User(parent_token.get());
  if (!IsWellKnownSid(const_cast<BYTE*>(parent_sid.data()), WinLocalSystemSid)) return ERROR_ACCESS_DENIED;
  const Handle pipe(CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  DWORD actual_parent = 0;
  if (pipe.get() == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe.get(), &actual_parent) ||
      actual_parent != pid || WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
  Request request;
  for (size_t offset = 0; offset < sizeof(request);) {
    if (!Remaining(deadline, 1) || WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT) return ERROR_TIMEOUT;
    DWORD available = 0, got = 0;
    if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr) || available > sizeof(request) - offset)
      return ERROR_INVALID_DATA;
    if (!available) { Sleep(1); continue; }
    if (!ReadFile(pipe.get(), reinterpret_cast<BYTE*>(&request) + offset, available, &got, nullptr) || !got)
      return ERROR_READ_FAULT;
    offset += got;
  }
  DWORD session = 0;
  if (request.magic != kMagic || request.version != kVersion || request.size != sizeof(request) || request.deadline != deadline ||
      request.variant >= std::size(kVariants) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session || session != request.session ||
      !Remaining(request.deadline, 1) || request.deadline - GetTickCount64() > 5000 ||
      request.private_bytes > sizeof(request.private_sid) || request.package_bytes > sizeof(request.package_sid) ||
      !BoundedSid(request.private_sid, request.private_bytes, request.private_sid) ||
      !BoundedSid(request.package_sid, request.package_bytes, request.package_sid)) return ERROR_INVALID_DATA;
  const Handle private_token(reinterpret_cast<HANDLE>(request.private_token));
  const Handle lowbox_token(reinterpret_cast<HANDLE>(request.lowbox_token));
  const Handle job(reinterpret_cast<HANDLE>(request.job));
  const Handle report_file(reinterpret_cast<HANDLE>(request.report));
  CheckToken(private_token.get(), request, false); CheckToken(lowbox_token.get(), request, true);
  BOOL assigned = FALSE;
  Require(IsProcessInJob(GetCurrentProcess(), job.get(), &assigned) && assigned,
      "owned-desktop-helper-original-job", ERROR_ACCESS_DENIED);
  BY_HANDLE_FILE_INFORMATION information{};
  Require(GetFileType(report_file.get()) == FILE_TYPE_DISK && GetFileInformationByHandle(report_file.get(), &information) &&
      information.nNumberOfLinks == 1 && !information.nFileSizeHigh && !information.nFileSizeLow &&
      !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)),
      "owned-desktop-report-leaf", ERROR_INVALID_DATA);
  const Report report = Execute(request);
  DWORD written = 0;
  return WriteFile(report_file.get(), &report, sizeof(report), &written, nullptr) && written == sizeof(report)
      ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
}
void Print(const Report& report) {
  if (report.variant) {
    std::printf("Owned desktop matrix: variant=%s effective station=%#lx/%#lx desktop=%#lx/%#lx\n",
        kVariants[report.variant].name, report.station_access[0], report.station_access[1],
        report.desktop_access[0], report.desktop_access[1]);
  }
  std::printf("Owned desktop diagnostic: setup=%lu session=%lu created=%lu/%lu verified=%lu restore=%lu close=%lu/%lu\n",
      report.setup, report.session, report.station_created, report.desktop_created, report.security_verified,
      report.restore, report.desktop_close, report.station_close);
  for (unsigned i = report.variant ? 2u : 0u; i < 4; ++i) {
    const auto& row = report.rows[i];
    std::printf("Owned desktop diagnostic: row=%u create=%lu resume=%lu wait=%lu exit=0x%08lx stop=%lu\n",
        i, row.create, row.resume, row.wait, row.exit, row.stop);
  }
}
void PrintStored(const std::wstring& directory, DWORD variant = 0) {
  try {
    const auto bytes = Read(directory + L"\\" + kVariants[variant].leaf, sizeof(Report));
    Report report;
    Require(bytes.size() == sizeof(report), "owned-desktop-stored-size", ERROR_INVALID_DATA);
    std::memcpy(&report, bytes.data(), sizeof(report));
    DWORD session = 0;
    Require(report.magic == kMagic && report.version == kVersion && report.size == sizeof(report) &&
        report.variant == variant && ProcessIdToSessionId(GetCurrentProcessId(), &session) && report.session == session &&
        report.station_created <= 1 && report.desktop_created <= 1 && report.security_verified <= 1,
        "owned-desktop-stored-shape", ERROR_INVALID_DATA);
    Print(report);
  } catch (const Failure& failure) {
    std::printf("Owned desktop diagnostic report unavailable: variant=%s win32=%lu\n", kVariants[variant].name, failure.status);
  } catch (...) { std::puts("Owned desktop diagnostic report unavailable: allocation failure"); }
}
DWORD SelfTest() {
  Request request;
  Require(ProcessIdToSessionId(GetCurrentProcessId(), &request.session) && request.session,
      "owned-desktop-selftest-interactive-session", ERROR_NOT_READY);
  request.deadline = GetTickCount64() + 5000;
  HANDLE raw = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "owned-desktop-selftest-token");
  const Handle token(raw); const auto sid = User(token.get());
  request.private_bytes = static_cast<DWORD>(sid.size());
  Require(request.private_bytes <= sizeof(request.private_sid) && CopySid(sizeof(request.private_sid), request.private_sid,
      const_cast<BYTE*>(sid.data())), "owned-desktop-selftest-user");
  PSID package = nullptr;
  Require(SUCCEEDED(DeriveAppContainerSidFromAppContainerName(L"Gesher.OwnedDesktop.MechanicsOnly", &package)),
      "owned-desktop-selftest-package");
  const auto free_sid = [](void* p) { if (p) FreeSid(p); };
  const std::unique_ptr<void, decltype(free_sid)> own(package, free_sid);
  request.package_bytes = GetLengthSid(package);
  Require(request.package_bytes <= sizeof(request.package_sid) && CopySid(sizeof(request.package_sid), request.package_sid, package),
      "owned-desktop-selftest-package-copy");
  const Report report = Execute(request, true);
  std::puts("Owned desktop selftest: ordinary desktop-only mechanics; named station/private/lowbox paths unverified");
  Print(report);
  // No private or lowbox claim: only current ordinary identity object/loader mechanics.
  return report.setup ? report.setup : report.restore ? report.restore :
      report.desktop_close ? report.desktop_close : report.station_close ? report.station_close :
      report.rows[0].create || report.rows[0].exit || report.rows[0].stop ||
      report.rows[2].create || report.rows[2].exit || report.rows[2].stop ? ERROR_DLL_INIT_FAILED : ERROR_SUCCESS;
}
void Observe(HANDLE source, HANDLE lowbox, const storage::AuthorityLaunchSpec& spec,
             PSID private_user, PSID package, GuestLoaderTrace* trace, DWORD variant = 0) {
  Report report;
  report.session = spec.desktop_session;
  report.variant = variant;
  DWORD helper_status = ERROR_NOT_READY;
  Handle process, thread, report_file;
  std::wstring report_path;
  try {
    Require(SystemCaller(), "owned-desktop-parent-system", ERROR_ACCESS_DENIED);
    report_path = config.directory + L"\\" + kVariants[variant].leaf;
    const Local descriptor = Security();
    SECURITY_ATTRIBUTES file_security{sizeof(file_security), descriptor.get(), FALSE};
    report_file.reset(CreateFileW(report_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        &file_security, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Require(report_file.get() != INVALID_HANDLE_VALUE, "owned-desktop-parent-report-create");
    Request request;
    request.session = spec.desktop_session;
    request.variant = variant;
    request.deadline = std::min(config.worker_deadline, GetTickCount64() + 5000);
    Require(Remaining(request.deadline, 1), "owned-desktop-parent-deadline", ERROR_TIMEOUT);
    request.private_bytes = GetLengthSid(private_user); request.package_bytes = GetLengthSid(package);
    Require(request.private_bytes <= sizeof(request.private_sid) && request.package_bytes <= sizeof(request.package_sid) &&
        CopySid(sizeof(request.private_sid), request.private_sid, private_user) &&
        CopySid(sizeof(request.package_sid), request.package_sid, package), "owned-desktop-parent-subjects");
    HANDLE raw = nullptr;
    Require(CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &raw),
        "owned-desktop-parent-private-token");
    const Handle private_token(raw);
    DWORD size = 0;
    GetTokenInformation(lowbox, TokenDefaultDacl, nullptr, 0, &size);
    Require(size && size <= 65536, "owned-desktop-parent-default-size", ERROR_INVALID_DATA);
    std::vector<BYTE> dacl(size);
    Require(GetTokenInformation(lowbox, TokenDefaultDacl, dacl.data(), size, &size) &&
        SetTokenInformation(private_token.get(), TokenDefaultDacl, dacl.data(), size), "owned-desktop-parent-default");
    raw = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &raw), "owned-desktop-parent-system-token");
    const Handle system_source(raw);
    raw = nullptr;
    Require(DuplicateTokenEx(system_source.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &raw),
        "owned-desktop-parent-system-primary");
    const Handle system_token(raw);
    // Only this new SYSTEM helper token moves sessions; the service and every
    // original token/privilege state remain unchanged.
    Require(SetTokenInformation(system_token.get(), TokenSessionId, &request.session, sizeof(request.session)),
        "owned-desktop-parent-helper-session");
    GUID unique{}; wchar_t suffix[40]{};
    Require(SUCCEEDED(CoCreateGuid(&unique)) && StringFromGUID2(unique, suffix, static_cast<int>(std::size(suffix))) > 0,
        "owned-desktop-parent-channel-guid");
    const std::wstring channel = L"\\\\.\\pipe\\Gesher.OwnedDesktop.Control." + std::wstring(suffix);
    PSECURITY_DESCRIPTOR raw_channel = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;GA;;;SY)",
        SDDL_REVISION_1, &raw_channel, nullptr), "owned-desktop-parent-channel-security");
    const Local channel_descriptor(raw_channel);
    SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), channel_descriptor.get(), FALSE};
    const Handle pipe(CreateNamedPipeW(channel.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &pipe_security));
    Require(pipe.get() != INVALID_HANDLE_VALUE, "owned-desktop-parent-fixed-pipe");
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "owned-desktop-parent-birth");
    const ULONGLONG birth = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    SIZE_T needed = 0; InitializeProcThreadAttributeList(nullptr, 2, 0, &needed);
    std::vector<BYTE> memory(needed);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    Require(InitializeProcThreadAttributeList(attributes, 2, 0, &needed), "owned-desktop-parent-attributes");
    const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST p) { DeleteProcThreadAttributeList(p); };
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(release)> own(attributes, release);
    HANDLE original_job = spec.job;
    DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
    Require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &original_job, sizeof(original_job), nullptr, nullptr) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigation, sizeof(mitigation), nullptr, nullptr),
        "owned-desktop-parent-atomic-job-mitigation");
    wchar_t encoded[32]{};
    Require(std::swprintf(encoded, std::size(encoded), L"%llx", birth) > 0, "owned-desktop-parent-birth-argument");
    std::wstring command = Quote(config.image) + L" --owned-desktop-control " + Quote(channel) + L" " +
        std::to_wstring(GetCurrentProcessId()) + L" " + encoded + L" " + std::to_wstring(request.deadline);
    std::vector<std::wstring> entries;
    for (size_t offset = 0; offset < spec.environment.size() && spec.environment[offset];) {
      entries.emplace_back(spec.environment.data() + offset); offset += entries.back().size() + 1;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> environment;
    for (const auto& entry : entries) { environment.insert(environment.end(), entry.begin(), entry.end()); environment.push_back(0); }
    if (entries.empty()) environment.push_back(0);
    environment.push_back(0);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    wchar_t desktop_name[] = L"winsta0\\default"; startup.StartupInfo.lpDesktop = desktop_name;
    PROCESS_INFORMATION child{};
    Require(CreateProcessAsUserW(system_token.get(), config.image.c_str(), command.data(), &file_security, &file_security,
        FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), spec.directory, &startup.StartupInfo, &child), "owned-desktop-parent-create-helper");
    process.reset(child.hProcess); thread.reset(child.hThread);
    // Windows forbids Session0-to-interactive handle inheritance. Duplicate
    // only into this retained suspended process; no process memory is patched.
    const auto deliver = [&](HANDLE handle, DWORD access) {
      HANDLE remote = nullptr;
      Require(DuplicateHandle(GetCurrentProcess(), handle, process.get(), &remote, access, FALSE, 0),
          "owned-desktop-parent-deliver-exact");
      return reinterpret_cast<ULONG_PTR>(remote);
    };
    request.private_token = deliver(private_token.get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY);
    request.lowbox_token = deliver(lowbox, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY);
    request.job = deliver(spec.job, JOB_OBJECT_QUERY);
    request.report = deliver(report_file.get(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    BOOL assigned = FALSE;
    Require(IsProcessInJob(process.get(), spec.job, &assigned) && assigned && ResumeThread(thread.get()) == 1,
        "owned-desktop-parent-verify-resume", ERROR_ACCESS_DENIED);
    for (;;) {
      if (ConnectNamedPipe(pipe.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) break;
      Require(GetLastError() == ERROR_PIPE_LISTENING && Remaining(request.deadline, 1) &&
          WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT, "owned-desktop-parent-connect", ERROR_TIMEOUT);
      Sleep(1);
    }
    DWORD actual_pid = 0, actual_session = 0;
    Require(GetNamedPipeClientProcessId(pipe.get(), &actual_pid) && actual_pid == GetProcessId(process.get()) &&
        GetNamedPipeClientSessionId(pipe.get(), &actual_session) && actual_session == request.session &&
        WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT, "owned-desktop-parent-exact-client", ERROR_ACCESS_DENIED);
    for (size_t offset = 0; offset < sizeof(request);) {
      Require(Remaining(request.deadline, 1) && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT,
          "owned-desktop-parent-delivery-deadline", ERROR_TIMEOUT);
      DWORD written = 0;
      const BOOL sent = WriteFile(pipe.get(), reinterpret_cast<const BYTE*>(&request) + offset,
          static_cast<DWORD>(sizeof(request) - offset), &written, nullptr);
      Require(sent || GetLastError() == ERROR_NO_DATA, "owned-desktop-parent-write-request");
      offset += written;
      if (!written) Sleep(1);
    }
    Require(WaitForSingleObject(process.get(), Remaining(request.deadline, 5000)) == WAIT_OBJECT_0,
        "owned-desktop-parent-wait", ERROR_TIMEOUT);
    Require(GetExitCodeProcess(process.get(), &helper_status), "owned-desktop-parent-helper-exit");
    Check(helper_status, "owned-desktop-parent-helper-result");
    report_file.reset();
    const auto bytes = Read(report_path, sizeof(report));
    Require(bytes.size() == sizeof(report), "owned-desktop-parent-report-size", ERROR_INVALID_DATA);
    std::memcpy(&report, bytes.data(), sizeof(report));
    Require(report.magic == kMagic && report.version == kVersion && report.size == sizeof(report) &&
        report.session == request.session && report.variant == variant &&
        report.station_created <= 1 && report.desktop_created <= 1 && report.security_verified <= 1,
        "owned-desktop-parent-report-shape", ERROR_INVALID_DATA);
  } catch (const Failure& failure) { report.setup = failure.status; }
    catch (...) { report.setup = ERROR_NOT_ENOUGH_MEMORY; }
  if (process && WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
    TerminateProcess(process.get(), ERROR_TIMEOUT);
    WaitForSingleObject(process.get(), Remaining(config.worker_deadline, 250));
  }
  if (report.setup && report_file && report_file.get() != INVALID_HANDLE_VALUE &&
      (!process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)) {
    // Only our retained CREATE_NEW leaf can receive a failed-bootstrap record,
    // and only after the sole helper writer is confirmed dead.
    LARGE_INTEGER zero{}; DWORD written = 0;
    if (SetFilePointerEx(report_file.get(), zero, nullptr, FILE_BEGIN) &&
        WriteFile(report_file.get(), &report, sizeof(report), &written, nullptr) && written == sizeof(report))
      SetEndOfFile(report_file.get());
  }
  if (variant) return; // matrix evidence stays in its separate bounded reports
  char text[768]{};
  int count = std::snprintf(text, sizeof(text), "Owned desktop control: setup=%lu helper=%lu session=%lu objects=%lu/%lu verified=%lu restore=%lu close=%lu/%lu; context-only, not ACL cause\n",
      report.setup, helper_status, report.session, report.station_created, report.desktop_created,
      report.security_verified, report.restore, report.desktop_close, report.station_close);
  for (unsigned i = 0; i < 4 && count > 0 && static_cast<size_t>(count) < sizeof(text); ++i) {
    const auto& row = report.rows[i];
    count += std::snprintf(text + count, sizeof(text) - static_cast<size_t>(count), "Owned desktop row=%u create=%lu resume=%lu wait=%lu exit=0x%08lx stop=%lu\n",
        i, row.create, row.resume, row.wait, row.exit, row.stop);
  }
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(text) || trace->bytes > sizeof(trace->tail)) return;
  const size_t added = static_cast<size_t>(count);
  // The separately read fixed report carries this control even when the tail
  // is full. Never evict an original USER32/GDI observation for the new rows.
  if (added > sizeof(trace->tail) - trace->bytes) return;
  std::memcpy(trace->tail + trace->bytes, text, added);
  trace->bytes += static_cast<DWORD>(added);
}
} // namespace owned_desktop_control
