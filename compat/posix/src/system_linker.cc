// gesher-base: Gesher's x86-64 ELF loader as /system/bin/linker64
// (system_linker.h, docs/decisions/0096-elf-loader-as-system-linker64.md).
//
// On a 64-bit Android device /system/bin/linker64 is the dynamic linker, an
// ELF file whose header names the device's machine, and every process maps it
// (/proc/self/maps names it). Gesher's dynamic linker is its ELF loader
// (compat/elf/elf_loader.cc), part of dalvikvm.exe and gesher-exec.exe; it
// loads x86-64 ELF natively. This file gives it that name: path.cc maps
// /system/bin/linker64 to a file holding Image(), procfs.cc maps that file
// before it lists /proc/self/maps and names it /system/bin/linker64 there.
// There is no /system/bin/linker: Gesher runs no 32-bit code.

#include "system_linker.h"

#include <errno.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal.h"

namespace gesher::system_linker {
namespace {

constexpr char kNoteName[] = "Gesher";
constexpr char kNoteText[] =
    "Gesher ELF loader: the dynamic linker of this process, part of Gesher's runtime. "
    "It loads x86-64 ELF natively. Not a program: exec of /system/bin/linker64 runs the "
    "program named after it.";
constexpr char kComment[] = "Gesher ELF loader";
constexpr uint32_t kNoteType = 1;

void Put16(std::string* out, size_t offset, uint16_t v) {
  for (int i = 0; i < 2; ++i) (*out)[offset + i] = static_cast<char>(v >> (8 * i));
}
void Put32(std::string* out, size_t offset, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*out)[offset + i] = static_cast<char>(v >> (8 * i));
}
void Put64(std::string* out, size_t offset, uint64_t v) {
  for (int i = 0; i < 8; ++i) (*out)[offset + i] = static_cast<char>(v >> (8 * i));
}
size_t Align(std::string* out, size_t align) {
  while (out->size() % align != 0) out->push_back('\0');
  return out->size();
}

std::string BuildImage() {
  constexpr size_t kEhdr = 64, kPhdr = 56, kShdr = 64;
  constexpr int kPhnum = 3, kShnum = 4;
  std::string out(kEhdr + kPhnum * kPhdr, '\0');

  // .note.gesher: namesz, descsz, type, name and text, each padded to 4.
  const size_t note = Align(&out, 4);
  out.append(12, '\0');
  Put32(&out, note, sizeof(kNoteName));
  Put32(&out, note + 4, sizeof(kNoteText));
  Put32(&out, note + 8, kNoteType);
  out.append(kNoteName, sizeof(kNoteName));
  Align(&out, 4);
  out.append(kNoteText, sizeof(kNoteText));
  Align(&out, 4);
  const size_t note_size = out.size() - note;

  const size_t comment = out.size();
  out.append(kComment, sizeof(kComment));

  constexpr char kShstr[] = "\0.note.gesher\0.comment\0.shstrtab";
  const size_t shstrtab = out.size();
  out.append(kShstr, sizeof(kShstr));
  const size_t shoff = Align(&out, 8);
  out.append(kShnum * kShdr, '\0');
  const size_t size = out.size();

  // ELF header: ELF64, little-endian, ET_DYN, EM_X86_64, no entry point.
  std::memcpy(out.data(), "\x7f" "ELF", 4);
  out[4] = 2;  // ELFCLASS64
  out[5] = 1;  // ELFDATA2LSB
  out[6] = 1;  // EV_CURRENT
  Put16(&out, 16, 3);   // ET_DYN
  Put16(&out, 18, 62);  // EM_X86_64
  Put32(&out, 20, 1);   // EV_CURRENT
  Put64(&out, 24, 0);   // e_entry
  Put64(&out, 32, kEhdr);
  Put64(&out, 40, shoff);
  Put16(&out, 52, kEhdr);
  Put16(&out, 54, kPhdr);
  Put16(&out, 56, kPhnum);
  Put16(&out, 58, kShdr);
  Put16(&out, 60, kShnum);
  Put16(&out, 62, 3);  // .shstrtab

  auto phdr = [&](int i, uint32_t type, uint32_t flags, uint64_t offset, uint64_t filesz,
                  uint64_t align) {
    const size_t p = kEhdr + i * kPhdr;
    Put32(&out, p, type);
    Put32(&out, p + 4, flags);
    Put64(&out, p + 8, offset);
    Put64(&out, p + 16, offset);  // p_vaddr: loaded at its file offset
    Put64(&out, p + 24, offset);
    Put64(&out, p + 32, filesz);
    Put64(&out, p + 40, filesz);
    Put64(&out, p + 48, align);
  };
  phdr(0, 1, 4, 0, size, 4096);              // PT_LOAD, R: the whole file, no code
  phdr(1, 4, 4, note, note_size, 4);         // PT_NOTE
  phdr(2, 0x6474e551, 6, 0, 0, 16);          // PT_GNU_STACK, RW

  auto shdr = [&](int i, uint32_t name, uint32_t type, uint64_t flags, uint64_t offset,
                  uint64_t sh_size, uint64_t align, uint64_t entsize) {
    const size_t s = shoff + i * kShdr;
    Put32(&out, s, name);
    Put32(&out, s + 4, type);
    Put64(&out, s + 8, flags);
    Put64(&out, s + 16, (flags & 2) != 0 ? offset : 0);
    Put64(&out, s + 24, offset);
    Put64(&out, s + 32, sh_size);
    Put64(&out, s + 48, align);
    Put64(&out, s + 56, entsize);
  };
  shdr(1, 1, 7, 2, note, note_size, 4, 0);                    // .note.gesher: SHT_NOTE, ALLOC
  shdr(2, 14, 1, 0x30, comment, sizeof(kComment), 1, 1);      // .comment: MERGE|STRINGS
  shdr(3, 23, 3, 0, shstrtab, sizeof(kShstr), 1, 0);          // .shstrtab
  return out;
}

std::wstring StripPrefix(const std::wstring& path) {
  if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
  if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
  return path;
}

std::wstring Environment(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0) return std::wstring();
  std::wstring value(size, L'\0');
  DWORD n = GetEnvironmentVariableW(name, value.data(), size);
  if (n == 0 || n >= size) return std::wstring();
  value.resize(n);
  return value;
}

bool CanPublish() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD contained = 0, size = 0;
  const bool queried = GetTokenInformation(token, TokenIsAppContainer, &contained,
                                           sizeof(contained), &size) != FALSE;
  CloseHandle(token);
  return queried && contained == 0;
}

// $ANDROID_DATA (a Windows or a Linux path), else %LOCALAPPDATA%\Gesher\data
// as the launcher's default.
std::wstring DataDirectory() {
  std::wstring data = Environment(L"ANDROID_DATA");
  if (!data.empty()) {
    std::wstring mapped;
    const std::string narrow = Narrow(data.c_str());
    if (MapPath(narrow.c_str(), &mapped) != PathKind::kFile) return std::wstring();
    data = StripPrefix(mapped);
  } else {
    data = Environment(L"LOCALAPPDATA");
    if (data.empty()) return std::wstring();
    data += L"\\Gesher\\data";
  }
  for (wchar_t& c : data) {
    if (c == L'/') c = L'\\';
  }
  return data;
}

void MakeDirectories(const std::wstring& dir) {
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == L'\\') {
      const std::wstring part = dir.substr(0, i);
      if (!part.empty() && part.back() != L':' && part != L"\\") CreateDirectoryW(part.c_str(), nullptr);
    }
  }
}

bool ReadAll(const std::wstring& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 65536;
  if (ok) {
    out->assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                          got == out->size());
  }
  CloseHandle(h);
  return ok;
}

// Makes `path` a file holding exactly `image`. An existing file with these
// bytes is used as it is (other processes may have it mapped). It is not
// marked read-only: the name refuses changes (EROFS, IsName), and a read-only
// file would stop ordinary cleanups of Gesher's data directory.
bool Place(const std::wstring& path, const std::string& image) {
  std::string existing;
  if (ReadAll(path, &existing) && existing == image) {
    // Written read-only by the first version of this code.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
      SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    return true;
  }
  const std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(h, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) &&
                  written == image.size();
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(temp.c_str());
    return false;
  }
  if (MoveFileExW(temp.c_str(), path.c_str(), 0)) return true;
  // Another process placed it first, or the file there holds other bytes.
  if (ReadAll(path, &existing) && existing == image) {
    DeleteFileW(temp.c_str());
    return true;
  }
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
  DeleteFileW(temp.c_str());
  return false;
}

bool IsHexName(const wchar_t* name) {
  size_t n = 0;
  for (; name[n] != L'\0'; ++n) {
    if (!iswxdigit(name[n])) return false;
  }
  return n == 8;
}

// The directory is in use: its last-write time becomes now, so that another
// process's Prune does not take it for an abandoned one.
void MarkUsed(const std::wstring& dir) {
  HANDLE h = CreateFileW(dir.c_str(), FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  SetFileTime(h, nullptr, nullptr, &now);
  CloseHandle(h);
}

bool ProcessRuns(DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;  // it exists
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}

// Removes what earlier images and dead processes left under `root`: other
// image directories unused for a week (a process still running from one keeps
// its mapping; only a name it opened again later would be gone) and temporary
// files of processes that no longer run.
void Prune(const std::wstring& root, const std::wstring& keep) {
  constexpr unsigned long long kWeek = 7ull * 24 * 3600 * 10000000ull;  // 100 ns units
  FILETIME now_time;
  GetSystemTimeAsFileTime(&now_time);
  const unsigned long long now =
      (static_cast<unsigned long long>(now_time.dwHighDateTime) << 32) | now_time.dwLowDateTime;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || !IsHexName(found.cFileName)) {
      continue;
    }
    const std::wstring sub = root + L"\\" + found.cFileName;
    if (_wcsicmp(found.cFileName, keep.c_str()) == 0) {
      WIN32_FIND_DATAW temp;
      HANDLE t = FindFirstFileW((sub + L"\\linker64.*.tmp").c_str(), &temp);
      if (t == INVALID_HANDLE_VALUE) continue;
      do {
        const DWORD pid = static_cast<DWORD>(std::wcstoul(temp.cFileName + 9, nullptr, 10));
        if (!ProcessRuns(pid)) DeleteFileW((sub + L"\\" + temp.cFileName).c_str());
      } while (FindNextFileW(t, &temp));
      FindClose(t);
      continue;
    }
    const unsigned long long written =
        (static_cast<unsigned long long>(found.ftLastWriteTime.dwHighDateTime) << 32) |
        found.ftLastWriteTime.dwLowDateTime;
    if (now < written || now - written < kWeek) continue;
    WIN32_FIND_DATAW entry;
    HANDLE e = FindFirstFileW((sub + L"\\*").c_str(), &entry);
    if (e != INVALID_HANDLE_VALUE) {
      do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::wstring file = sub + L"\\" + entry.cFileName;
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(file.c_str());
      } while (FindNextFileW(e, &entry));
      FindClose(e);
    }
    RemoveDirectoryW(sub.c_str());
  } while (FindNextFileW(find, &found));
  FindClose(find);
}

std::wstring FinalPath(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::wstring();
  std::wstring final(32768, L'\0');
  DWORD n = GetFinalPathNameByHandleW(h, final.data(), static_cast<DWORD>(final.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(h);
  if (n == 0 || n >= final.size()) return std::wstring();
  final.resize(n);
  return StripPrefix(final);
}

// The file and the mapping are made once, under these locks rather than
// std::call_once, so that a guest fork (ADR 0092) can hold them: a forked
// child never inherits one half made by a thread it does not have.
std::mutex g_file_mutex;
std::atomic<bool> g_file_ready{false};
std::mutex g_mapped_mutex;
std::atomic<bool> g_mapped{false};
std::wstring g_file;

std::wstring HashName() {
  const std::string image = BuildImage();
  uint32_t fnv = 2166136261u;
  for (unsigned char c : image) fnv = (fnv ^ c) * 16777619u;
  wchar_t name[16];
  std::swprintf(name, 16, L"%08x", fnv);
  return name;
}

// The file holding the image, as this process knows it: its final path and
// its file identity (volume serial number and file index). Calls that change
// a file through another name (a symbolic link, an fd) compare identities.
struct Identity {
  bool known = false;
  std::wstring path;
  std::wstring nt_path;
  FILE_ID_INFO id = {};
};

// The file's identity: FileIdInfo (64-bit volume serial number, 128-bit file
// ID, unique on ReFS and Dev Drives too); on a file system without it (FAT),
// the 32-bit serial number and 64-bit index widened to that form. A handle
// without access to attributes (opened with no access) is reopened for them.
bool QueryId(HANDLE h, FILE_ID_INFO* id) {
  if (GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof(*id))) return true;
  BY_HANDLE_FILE_INFORMATION info;
  if (GetFileInformationByHandle(h, &info)) {
    *id = {};
    id->VolumeSerialNumber = info.dwVolumeSerialNumber;
    const uint64_t index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    std::memcpy(id->FileId.Identifier, &index, sizeof(index));
    return true;
  }
  if (GetLastError() != ERROR_ACCESS_DENIED) return false;
  HANDLE again = ReOpenFile(h, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_FLAG_BACKUP_SEMANTICS);
  if (again == INVALID_HANDLE_VALUE) return false;
  const bool ok = QueryId(again, id);
  CloseHandle(again);
  return ok;
}

bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
  return a.VolumeSerialNumber == b.VolumeSerialNumber &&
         std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
}
std::mutex g_identity_mutex;
bool g_identity_probed = false;
Identity g_identity;

void LearnLocked(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  FILE_ID_INFO id;
  if (QueryId(h, &id)) {
    g_identity.known = true;
    g_identity.id = id;
    std::wstring final(32768, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, final.data(), static_cast<DWORD>(final.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n != 0 && n < final.size()) {
      final.resize(n);
      g_identity.path = StripPrefix(final);
    }
    final.resize(32768);
    n = GetFinalPathNameByHandleW(h, final.data(), static_cast<DWORD>(final.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (n != 0 && n < final.size()) {
      final.resize(n);
      g_identity.nt_path = std::move(final);
    }
  }
  CloseHandle(h);
}

// The identity, from File() when this process used the name, otherwise from
// the file where File() would put it, looked up without writing it: once, or
// again when `reprobe` (a write refused for sharing, which a process holding
// the file causes) and it was not found before.
template <typename F>
bool WithIdentity(F f, bool reprobe = false) {
  std::lock_guard<std::mutex> lock(g_identity_mutex);
  if (!g_identity.known && (!g_identity_probed || reprobe)) {
    g_identity_probed = true;
    if (g_file_ready.load(std::memory_order_acquire)) {
      if (!g_file.empty()) LearnLocked(g_file);
    } else {
      const std::wstring data = DataDirectory();
      if (!data.empty()) {
        LearnLocked(data + L"\\system\\gesher-linker\\" + HashName() + L"\\linker64");
      }
    }
  }
  return g_identity.known && f(g_identity);
}

}  // namespace

std::string Image() {
  static const std::string image = BuildImage();
  return image;
}

const std::wstring& File() {
  if (g_file_ready.load(std::memory_order_acquire)) return g_file;
  std::lock_guard<std::mutex> once(g_file_mutex);
  if (g_file_ready.load(std::memory_order_relaxed)) return g_file;
  [] {
    const std::wstring data = DataDirectory();
    if (!data.empty()) {
      const std::string image = Image();
      // A directory per image: a process that has an older one mapped keeps
      // it, and a file is never rewritten while mapped.
      const std::wstring name = HashName();
      const std::wstring root = data + L"\\system\\gesher-linker";
      const std::wstring dir = root + L"\\" + name;
      const std::wstring path = dir + L"\\linker64";
      if (!CanPublish()) {
        // The desktop supervisor prepares this exact image and grants only
        // read access. Never place, touch or prune files from a guest.
        std::string existing;
        if (ReadAll(path, &existing) && existing == image) {
          g_file = path;
          std::lock_guard<std::mutex> lock(g_identity_mutex);
          g_identity_probed = true;
          LearnLocked(path);
        }
        return;
      }
      MakeDirectories(dir);
      MarkUsed(dir);
      if (Place(path, image)) {
        g_file = FinalPath(path);
        // Held for the process's life without sharing write access: while a
        // process that uses the name runs, no other name or process can open
        // the file for writing (OpenFile reports EROFS for it).
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::lock_guard<std::mutex> lock(g_identity_mutex);
        g_identity = Identity();
        g_identity_probed = true;
        if (!g_file.empty()) LearnLocked(g_file);
      }
      Prune(root, name);
    }
  }();
  g_file_ready.store(true, std::memory_order_release);
  return g_file;
}

const std::wstring& Publish() {
  static const std::wstring unavailable;
  return CanPublish() ? File() : unavailable;
}

bool IsName(const char* path) {
  if (path == nullptr || path[0] != '/' || path[1] == '/') return false;
  const size_t length = std::strlen(path);
  if (path[length - 1] == '/') return false;  // a directory: ENOTDIR on Linux
  // The components, walked as the kernel walks them: empty and "." ones are
  // skipped and ".." goes back, but only through system and bin (another
  // directory does not exist there: ENOENT), and nothing follows linker64
  // ("linker64/." is ENOTDIR).
  static constexpr std::string_view kParts[] = {"system", "bin", "linker64"};
  size_t count = 0;
  std::string_view rest(path);
  while (!rest.empty()) {
    const size_t slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    if (part.empty()) continue;
    if (count == 3) return false;
    if (part == ".") continue;
    if (part == "..") {
      if (count > 0) --count;
      continue;
    }
    if (part != kParts[count]) return false;
    ++count;
  }
  return count == 3;
}

int RefuseChange(const char* path) {
  if (!IsName(path)) return 0;
  errno = EROFS;
  return -1;
}

bool IsFile(const std::wstring& windows) {
  const std::wstring path = StripPrefix(windows);
  return WithIdentity([&](const Identity& id) {
    return !id.path.empty() && _wcsicmp(path.c_str(), id.path.c_str()) == 0;
  });
}

bool IsMappedNtName(const std::wstring& name) {
  return WithIdentity([&](const Identity& id) {
    return !id.nt_path.empty() && id.nt_path == name;
  });
}

bool IsFileHandle(HANDLE h, bool reprobe) {
  if (h == nullptr || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return false;
  // Most handles are not the file: only query one when there is a file.
  if (!WithIdentity([](const Identity&) { return true; }, reprobe)) return false;
  FILE_ID_INFO id;
  if (!QueryId(h, &id)) return false;
  return WithIdentity([&](const Identity& known) { return SameId(id, known.id); });
}

bool IsFilePath(const std::wstring& windows, bool follow, bool reprobe) {
  HANDLE h = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool is = IsFileHandle(h, reprobe);
  CloseHandle(h);
  return is;
}

int RefuseChangeHandle(HANDLE h) {
  if (!IsFileHandle(h)) return 0;
  errno = EROFS;
  return -1;
}

void EnsureMapped() {
  if (g_mapped.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> once(g_mapped_mutex);
  if (g_mapped.load(std::memory_order_relaxed)) return;
  [] {
    const std::wstring& file = File();
    if (file.empty()) return;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    HANDLE section = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(h);
    if (section == nullptr) return;
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(section);
    if (view == nullptr) return;
    // What is mapped is what the name promises; the view stays for the
    // process's lifetime, as the linker's mapping does on a device.
    const std::string image = Image();
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(view, &info, sizeof(info)) != sizeof(info) || info.RegionSize < image.size() ||
        std::memcmp(view, image.data(), image.size()) != 0) {
      UnmapViewOfFile(view);
    }
  }();
  g_mapped.store(true, std::memory_order_release);
}

}  // namespace gesher::system_linker

// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif
#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h): the loader's identity that stat and open of
// /system/bin/linker64 report, in a forked child.
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::system_linker::g_identity_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::system_linker::g_file_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::system_linker::g_mapped_mutex);
#endif
