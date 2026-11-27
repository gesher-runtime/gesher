// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#include "legacy_keystore_archive.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
constexpr wchar_t kStore[] = L".gesher-keystore";
constexpr wchar_t kLock[] = L".gesher-keystore.lock";

void Check(bool result, const char* operation) {
  if (!result) throw std::runtime_error(operation);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Local {
  void* value = nullptr;
  ~Local() { if (value) LocalFree(value); }
};

Bytes Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "open exact fixture file");
  return Bytes(std::istreambuf_iterator<char>(file), {});
}
void Write(const fs::path& path, const Bytes& data) {
  std::ofstream file(path, std::ios::binary);
  Check(file.good(), "create fixture file");
  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  file.close();
  Check(file.good(), "write exact fixture bytes");
}
void U32(Bytes& output, uint32_t value) {
  for (int bit : {24, 16, 8, 0}) output.push_back(static_cast<unsigned char>(value >> bit));
}
void Utf(Bytes& output, std::string_view value) {
  output.push_back(0);
  output.push_back(static_cast<unsigned char>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}
void Blob(Bytes& output, const Bytes& value) {
  U32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}
void Record(Bytes& output, std::string_view alias, unsigned char kind, int flag,
            const Bytes& key, const std::vector<Bytes>& certificates) {
  Utf(output, alias);
  output.push_back(kind);
  U32(output, 0x00010203);
  U32(output, 0x04050607);
  Utf(output, kind == 1 ? "AES" : kind == 2 ? "RSA" : "");
  if (flag >= 0) output.push_back(static_cast<unsigned char>(flag));
  Blob(output, key);
  U32(output, static_cast<uint32_t>(certificates.size()));
  for (const auto& certificate : certificates) Blob(output, certificate);
}
Bytes Store(bool gks1, bool raw, bool malformed = false) {
  // Synthetic key/certificate frames. The JVM fixture separately checks
  // real X.509 decoding and lookup through the production provider.
  Bytes data;
  U32(data, gks1 ? 0x474b5331 : 0x474b5332);
  U32(data, gks1 ? 3 : 4);
  const int legacy = gks1 ? -1 : raw ? 1 : 2;
  Record(data, "old-secret", 1, legacy, raw ? Bytes{0xa1, 0xa2, 0xa3} : Bytes{}, {});
  Record(data, "old-private", 2, legacy, raw ? Bytes{0xb1, 0xb2} : Bytes{}, {{0x31, 0x32}, {0x33}});
  Record(data, "certificate", 3, gks1 ? -1 : 0, malformed ? Bytes{0xc1} : Bytes{}, {{0x41, 0x42}});
  if (!gks1) Record(data, "modern", 1, 0, {0xd1, 0xd2}, {});
  return data;
}
Bytes ScrubbedGks1() {
  Bytes data;
  U32(data, 0x474b5332);
  U32(data, 3);
  Record(data, "old-secret", 1, 2, {}, {});
  Record(data, "old-private", 2, 2, {}, {{0x31, 0x32}, {0x33}});
  Record(data, "certificate", 3, 0, {}, {{0x41, 0x42}});
  return data;
}
uint32_t Migrate(const fs::path& data, const fs::path& archive, char16_t tag) {
  const uint32_t result = gesher::keystore::ArchiveLegacy(data.u16string(), archive.u16string(),
                                                         std::u16string(64, tag));
  if (result != ERROR_SUCCESS) std::printf("ArchiveLegacy Win32 %lu\n", static_cast<unsigned long>(result));
  return result;
}
fs::path ExactArchive(const fs::path& archive, wchar_t tag, const Bytes& expected) {
  const fs::path bucket = archive / std::wstring(64, tag);
  fs::path file;
  for (const auto& entry : fs::directory_iterator(bucket)) {
    Check(file.empty(), "one immutable archive for the same original");
    Check(entry.is_regular_file() && entry.path().extension() == L".gks", "only archive payload present");
    file = entry.path();
  }
  Check(!file.empty() && Read(file) == expected, "exact original bytes retained");
  return file;
}
bool HasArchive(const fs::path& archive, wchar_t tag) {
  return fs::exists(archive / std::wstring(64, tag));
}

std::wstring CurrentUserSid() {
  Handle token;
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value), "open fixture token");
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> info(size);
  Check(GetTokenInformation(token.value, TokenUser, info.data(), size, &size), "read fixture token");
  Local sid;
  Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid,
      reinterpret_cast<wchar_t**>(&sid.value)), "format fixture SID");
  return static_cast<wchar_t*>(sid.value);
}
class DenyNewFiles {
 public:
  explicit DenyNewFiles(const fs::path& path) : path_(path) {
    const std::wstring user = CurrentUserSid();
    const std::wstring sddl = L"D:P(D;;0x2;;;" + user + L")(A;;FA;;;" + user + L")(A;;FA;;;SY)";
    Local descriptor;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr), "build restricted test ACL");
    constexpr SECURITY_INFORMATION query = DACL_SECURITY_INFORMATION;
    DWORD size = 0;
    GetFileSecurityW(path_.c_str(), query, nullptr, 0, &size);
    original_.resize(size);
    Check(GetFileSecurityW(path_.c_str(), query, original_.data(), size, &size), "save fixture ACL");
    Check(SetFileSecurityW(path_.c_str(), query | PROTECTED_DACL_SECURITY_INFORMATION, descriptor.value),
          "deny new files in fixture");
  }
  ~DenyNewFiles() { SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, original_.data()); }
 private:
  fs::path path_;
  Bytes original_;
};

void Junction(const fs::path& link, const fs::path& target) {
  Check(fs::create_directory(link), "create owned junction directory");
  Handle file{CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  Check(file.value != INVALID_HANDLE_VALUE, "open owned junction directory");
  const std::wstring native = L"\\??\\" + target.wstring(), print = target.wstring();
  struct Header {
    DWORD tag;
    WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
  } header{IO_REPARSE_TAG_MOUNT_POINT, 0, 0, 0, static_cast<WORD>(native.size() * sizeof(wchar_t)),
      static_cast<WORD>((native.size() + 1) * sizeof(wchar_t)),
      static_cast<WORD>(print.size() * sizeof(wchar_t))};
  const size_t bytes = sizeof(header) + (native.size() + print.size() + 2) * sizeof(wchar_t);
  header.length = static_cast<WORD>(bytes - 8);
  Bytes buffer(bytes);
  std::memcpy(buffer.data(), &header, sizeof(header));
  std::memcpy(buffer.data() + sizeof(header), native.c_str(), header.substitute_length);
  std::memcpy(buffer.data() + sizeof(header) + header.print_offset, print.c_str(), header.print_length);
  DWORD returned = 0;
  Check(DeviceIoControl(file.value, FSCTL_SET_REPARSE_POINT, buffer.data(),
      static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr), "create actual junction");
}

struct Profile {
  std::wstring name;
  PSID sid = nullptr;
  explicit Profile(wchar_t role) : name(L"Gesher.KeyArchiveTest." + std::to_wstring(GetCurrentProcessId()) +
      L"." + std::to_wstring(GetTickCount64()) + role) {
    Check(SUCCEEDED(CreateAppContainerProfile(name.c_str(), name.c_str(), name.c_str(), nullptr, 0, &sid)) && sid,
          "create disposable AppContainer profile");
  }
  ~Profile() { FreeSid(sid); DeleteAppContainerProfile(name.c_str()); }
};

struct LowboxProcess {
  Handle process, thread;
  LowboxProcess(const Profile& profile, const fs::path& image, std::wstring arguments = {}) {
    SECURITY_CAPABILITIES capabilities{profile.sid, nullptr, 0, 0};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    Bytes attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    Check(InitializeProcThreadAttributeList(list, 1, 0, &size), "prepare lowbox process attributes");
    struct AttributeList {
      LPPROC_THREAD_ATTRIBUTE_LIST value;
      ~AttributeList() { DeleteProcThreadAttributeList(value); }
    } release{list};
    Check(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
        &capabilities, sizeof(capabilities), nullptr, nullptr), "set actual AppContainer SID");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + image.wstring() + L"\" " + arguments;
    Check(CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr, &startup.StartupInfo, &child),
        "create actual suspended AppContainer");
    process.value = child.hProcess;
    thread.value = child.hThread;
  }
  ~LowboxProcess() {
    TerminateProcess(process.value, 0);
    WaitForSingleObject(process.value, 2000);
  }
};

void LowboxDenied(const fs::path& image, const fs::path& archive) {
  const Profile other(L'R');
  const LowboxProcess child(other, image);
  Handle primary, tested;
  Check(OpenProcessToken(child.process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &primary.value),
        "own exact lowbox token");
  DWORD lowbox = 0, used = 0;
  Check(GetTokenInformation(primary.value, TokenIsAppContainer, &lowbox, sizeof(lowbox), &used) &&
      lowbox == 1, "verify actual AppContainer token");
  Check(DuplicateTokenEx(primary.value, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
      SecurityImpersonation, TokenImpersonation, &tested.value), "duplicate exact lowbox token for access test");
  Check(SetThreadToken(nullptr, tested.value), "apply exact lowbox token to archive open");
  const HANDLE read = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD read_error = GetLastError();
  const HANDLE write_dacl = CreateFileW(archive.c_str(), WRITE_DAC, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD dacl_error = GetLastError();
  const bool reverted = RevertToSelf() != FALSE;
  Handle received_read{read}, received_dacl{write_dacl};
  Check(reverted, "restore test owner token");
  Check(read == INVALID_HANDLE_VALUE && read_error == ERROR_ACCESS_DENIED, "lowbox cannot read legacy archive");
  Check(write_dacl == INVALID_HANDLE_VALUE && dacl_error == ERROR_ACCESS_DENIED, "same-user lowbox cannot change archive DACL");
  Check(!Read(archive).empty(), "actual trusted host can still read archived bytes");
}

void Grant(const fs::path& path, PSID sid, DWORD access, DWORD inheritance = NO_INHERITANCE) {
  Local descriptor;
  PACL current = nullptr;
  std::wstring native = path.wstring();
  Check(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
      &current, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value)) == ERROR_SUCCESS,
      "read owned fixture ACL");
  EXPLICIT_ACCESSW entry{};
  entry.grfAccessPermissions = access;
  entry.grfAccessMode = GRANT_ACCESS;
  entry.grfInheritance = inheritance;
  BuildTrusteeWithSidW(&entry.Trustee, sid);
  Local dacl;
  Check(SetEntriesInAclW(1, &entry, current, reinterpret_cast<PACL*>(&dacl.value)) == ERROR_SUCCESS,
        "prepare explicit test lowbox grant");
  Check(SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
      static_cast<PACL>(dacl.value), nullptr) == ERROR_SUCCESS, "grant fixture lowbox");
}

void PrivateLowboxCannotMigrateDirectly(const fs::path& image, const fs::path& test_root,
                                        const Bytes& original) {
  const Profile writer(L'W');
  Local label;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)",
      SDDL_REVISION_1, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&label.value), nullptr),
      "prepare same low mandatory label as private runtime");
  SECURITY_ATTRIBUTES security{sizeof(security), label.value, FALSE};
  Check(CreateDirectoryW(test_root.c_str(), &security), "create private host's low-labeled tree");
  using fs::path;
  const path data = test_root / L"data", system = test_root / L"system", archive = system / L"archive";
  fs::create_directory(data);
  fs::create_directory(system);
  Write(data / kStore, original);
  Grant(image, writer.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  Grant(test_root.parent_path(), writer.sid, FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE);
  Grant(test_root, writer.sid, FILE_ALL_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
  const LowboxProcess child(writer, image,
      L"--private-direct-probe \"" + data.wstring() + L"\" \"" + archive.wstring() + L"\"");
  Check(ResumeThread(child.thread.value) != DWORD(-1), "start actual private lowbox probe");
  Check(WaitForSingleObject(child.process.value, 15000) == WAIT_OBJECT_0,
        "wait for bounded private lowbox probe");
  DWORD code = 0;
  Check(GetExitCodeProcess(child.process.value, &code), "read exact private lowbox exit");
  Check(code == ERROR_ACCESS_DENIED && Read(data / kStore) == original && !fs::exists(archive),
        "private lowbox cannot migrate directly or alter original records");
  std::puts("PASS actual same-user lowbox can open its granted data and cannot migrate directly");
}
}  // namespace

int wmain(int argc, wchar_t** argv) try {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--private-direct-probe") {
    Handle token;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value), "private writer token");
    DWORD lowbox = 0, used = 0;
    Check(GetTokenInformation(token.value, TokenIsAppContainer, &lowbox, sizeof(lowbox), &used) &&
        lowbox == 1, "private writer really is an AppContainer");
    const fs::path data = argv[2];
    Check(Read(data / kStore) == Store(true, true), "private lowbox can read exact granted fixture file");
    return static_cast<int>(Migrate(data, argv[3], u'9'));
  }
  Check(argc == 2, "pass one owned scratch directory");
  const fs::path root = fs::absolute(argv[1]);
  const fs::path system = root / L"system";
  fs::create_directories(system);
  const fs::path archive = system / L"legacy-key-archive";
  auto data = [&](std::wstring_view name) {
    const fs::path path = root / name;
    Check(fs::create_directory(path), "create isolated fixture data directory");
    return path;
  };

  const Bytes old = Store(true, true), mixed = Store(false, true);
  const fs::path first = data(L"gks1");
  Write(first / kStore, old);
  Check(Migrate(first, archive, u'1') == ERROR_SUCCESS, "archive GKS1");
  Check(Read(first / kStore) == ScrubbedGks1(), "GKS1 preserves exact aliases and certificate frames");
  const fs::path original = ExactArchive(archive, L'1', old);
  Check(Migrate(first, archive, u'1') == ERROR_SUCCESS, "GKS1 migration is idempotent");
  Check(ExactArchive(archive, L'1', old) == original, "idempotent run retains existing archive entry");
  std::puts("PASS GKS1 exact history and idempotent scrub");

  const fs::path second = data(L"mixed");
  Write(second / kStore, mixed);
  Check(Migrate(second, archive, u'2') == ERROR_SUCCESS, "archive legacy flag in GKS2");
  Check(Read(second / kStore) == Store(false, false), "mixed store preserves modern raw and old metadata");
  ExactArchive(archive, L'2', mixed);
  std::puts("PASS mixed GKS2 legacy and modern isolation");

  const fs::path modern = data(L"modern"), missing = data(L"missing");
  const fs::path modern_file = modern / kStore;
  Write(modern_file, Store(false, false));
  Check(SetFileAttributesW(modern_file.c_str(), FILE_ATTRIBUTE_READONLY), "create real read-only store");
  uint32_t status = Migrate(modern, archive, u'3');
  Check(SetFileAttributesW(modern_file.c_str(), FILE_ATTRIBUTE_NORMAL), "restore fixture write attribute");
  Check(status == ERROR_SUCCESS && !fs::exists(modern / kLock) && !HasArchive(archive, L'3') &&
      Read(modern_file) == Store(false, false), "modern read-only store incurs no writes");
  {
    const DenyNewFiles read_only(missing);
    Check(Migrate(missing, archive, u'4') == ERROR_SUCCESS && !fs::exists(missing / kLock),
          "missing store incurs no writes in directory that denies file creation");
  }
  std::puts("PASS modern read-only and absent store no-op");

  const fs::path malformed = data(L"malformed");
  const Bytes bad = Store(true, true, true);
  Write(malformed / kStore, bad);
  Check(Migrate(malformed, archive, u'5') != ERROR_SUCCESS && Read(malformed / kStore) == bad &&
      !HasArchive(archive, L'5'), "malformed trusted record cannot retain hidden key bytes");
  const fs::path linked = data(L"hardlinked");
  Write(linked / L"other-file", old);
  fs::create_hard_link(linked / L"other-file", linked / kStore);
  Check(Migrate(linked, archive, u'6') != ERROR_SUCCESS && Read(linked / L"other-file") == old &&
      !HasArchive(archive, L'6'), "reject hardlink without writing to the other name");
  const fs::path other = data(L"root-for-junction"), outer = root / L"junction";
  const fs::path real_data = other / L"app";
  fs::create_directory(real_data);
  Write(real_data / kStore, old);
  Junction(outer, other);
  status = Migrate(outer / L"app", archive, u'7');
  Check(RemoveDirectoryW(outer.c_str()), "remove only the test junction");
  Check(status != ERROR_SUCCESS && Read(real_data / kStore) == old && !HasArchive(archive, L'7'),
        "deny traversing a reparse ancestor");
  const fs::path archive_alias = root / L"archive-junction";
  Junction(archive_alias, system);
  status = Migrate(real_data, archive_alias / archive.filename(), u'7');
  Check(RemoveDirectoryW(archive_alias.c_str()), "remove only the test archive junction");
  Check(status != ERROR_SUCCESS && Read(real_data / kStore) == old && !HasArchive(archive, L'7'),
        "deny traversing a reparse archive ancestor");
  std::puts("PASS malformed trusted, hardlink and intermediate junction");

  const fs::path rollback = data(L"rollback");
  Write(rollback / kStore, old);
  Write(rollback / kLock, {});
  {
    const DenyNewFiles reject_replacement(rollback);
    Check(Migrate(rollback, archive, u'8') != ERROR_SUCCESS && Read(rollback / kStore) == old,
          "failed scrub preserves original visible bytes");
    ExactArchive(archive, L'8', old);
  }
  Check(Migrate(rollback, archive, u'8') == ERROR_SUCCESS && Read(rollback / kStore) == ScrubbedGks1(),
      "retry reuses durable archive and scrubs only original");
  ExactArchive(archive, L'8', old);
  std::puts("PASS archive before failed scrub and subsequent recovery");

  LowboxDenied(fs::absolute(argv[0]), original);
  std::puts("PASS actual same-user AppContainer denied archive read and DACL mutation");
  PrivateLowboxCannotMigrateDirectly(fs::absolute(argv[0]), root / L"private-host", old);
  return 0;
} catch (const std::exception& failure) {
  std::fprintf(stderr, "legacy keystore fixture: %s (last Win32 %lu)\n", failure.what(), GetLastError());
  return 1;
}
