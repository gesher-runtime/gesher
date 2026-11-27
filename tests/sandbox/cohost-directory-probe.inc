// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

std::u16string DirectoryName(HANDLE directory, DWORD volume) {
  std::wstring name(32768, L'\0');
  const DWORD size = GetFinalPathNameByHandleW(directory, name.data(),
      static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | volume);
  if (size == 0 || size >= name.size()) return {};
  name.resize(size);
  if (volume == VOLUME_NAME_DOS && name.starts_with(L"\\\\?\\")) name.erase(0, 4);
  return U16(name);
}

bool ReadDirectoryNames(DIR* stream, std::vector<std::string>* names) {
  if (!stream) return false;
  errno = 0;
  while (const auto* entry = readdir(stream)) names->emplace_back(entry->d_name);
  const bool eof = errno == 0;
  return closedir(stream) == 0 && eof;
}

bool CheckCohostDirectory(const std::wstring& credential) {
  if (credential.size() < 3 || credential[1] != L':') return false;
  const auto path = fs::path(credential).generic_u8string();
  std::string root = "/mnt/";
  root.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(path[0]))));
  root.append(path.begin() + 2, path.end());
  const std::string original = root + "/scan";
  const std::string moved = root + "/moved";
  HANDLE direct = CreateFileW((fs::path(credential) / L"scan").c_str(), FILE_LIST_DIRECTORY,
      kSharing, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  const DWORD denial = GetLastError();
  if (direct != INVALID_HANDLE_VALUE) CloseHandle(direct);
  if (!Check(direct == INVALID_HANDLE_VALUE && denial == ERROR_ACCESS_DENIED,
             "lowbox cannot directly reopen the real directory path")) return false;
  const int directory = open(original.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (!Check(directory >= 0, "rooted storage gives the lowbox an exact directory descriptor")) return false;

  std::array<unsigned char, 24> first{}, second{};
  const int alias = dup(directory);
  if (!Check(alias >= 0, "lowbox duplicates its rooted directory descriptor")) return false;
  if (!Check(getdents64(directory, first.data(), first.size()) == first.size(),
             "private Binder getdents reads first directory entry")) return false;
  if (!Check(lseek(alias, 0, SEEK_CUR) == 1 &&
             getdents64(alias, second.data(), second.size()) == second.size() &&
             lseek(directory, 0, SEEK_CUR) == 2 && first != second,
             "private Binder getdents shares the directory offset across dup")) return false;
  if (!Check(lseek(alias, 0, SEEK_SET) == 0 &&
             getdents64(directory, second.data(), second.size()) == second.size() &&
             first == second, "rewinding an alias replays the original dirent")) return false;

  const int metadata = open(original.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  HANDLE unexpected = nullptr;
  if (!Check(metadata >= 0 &&
             getdents64(metadata, first.data(), first.size()) == -1 && errno == EBADF &&
             fdopendir(metadata) == nullptr && errno == EBADF &&
             gesher::binder_endpoint::ReopenDirectory(
                 reinterpret_cast<HANDLE>(_get_osfhandle(metadata)), &unexpected) == EACCES &&
             unexpected == INVALID_HANDLE_VALUE && close(metadata) == 0,
             "O_PATH descriptors cannot gain directory read rights")) return false;
  const int file = open((original + "/a").c_str(), O_RDONLY | O_CLOEXEC);
  if (!Check(file >= 0 && gesher::binder_endpoint::ReopenDirectory(
                 reinterpret_cast<HANDLE>(_get_osfhandle(file)), &unexpected) == ENOTDIR &&
             unexpected == INVALID_HANDLE_VALUE && close(file) == 0,
             "private Binder directory reopen rejects regular file descriptors")) return false;

  if (!Check(rename(original.c_str(), moved.c_str()) == 0 && mkdir(original.c_str(), 0700) == 0,
             "rooted rename and replacement succeed while original directory is open")) return false;
  const int replacement = open((original + "/x").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (!Check(replacement >= 0 && close(replacement) == 0 &&
             lseek(alias, 0, SEEK_SET) == 0, "replacement has its own content while old alias rewinds")) return false;
  std::vector<std::string> original_names, replacement_names;
  if (!Check(ReadDirectoryNames(fdopendir(alias), &original_names) &&
             lseek(directory, 0, SEEK_CUR) == static_cast<off_t>(original_names.size()) &&
             getdents64(directory, first.data(), first.size()) == 0 && close(directory) == 0 &&
             ReadDirectoryNames(opendir(original.c_str()), &replacement_names),
             "readdir and getdents use the held description and same Linux cursor")) return false;
  std::erase(original_names, ".");
  std::erase(original_names, "..");
  std::erase(replacement_names, ".");
  std::erase(replacement_names, "..");
  std::sort(original_names.begin(), original_names.end());
  return Check(original_names == std::vector<std::string>{"a", "b"} &&
               replacement_names == std::vector<std::string>{"x"},
               "renamed descriptor lists original identity; fresh lookup lists replacement");
}
