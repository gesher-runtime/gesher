/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <gesher/native_library_catalog.h>

#include <dirent.h>
#include <elf.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace gesher::nativeloader {
namespace {

struct FileCloser { void operator()(std::FILE* file) const { std::fclose(file); } };
struct DirectoryCloser { void operator()(DIR* directory) const { closedir(directory); } };
using File = std::unique_ptr<std::FILE, FileCloser>;

std::string StripSlash(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
      text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ReadConfig(const std::string& path, const std::string& directory,
                const std::string& company, elf::ProviderVisibility visibility, bool vendor,
                std::vector<PublicLibraryFile>* files, std::string* error) {
  File input(std::fopen(path.c_str(), "rb"));
  if (input == nullptr) {
    if (errno == ENOENT || errno == ENOTDIR) return true;
    *error = "Cannot read native public-library configuration " + path + ": " + std::strerror(errno);
    return false;
  }
  std::string contents;
  char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), input.get())) != 0) {
    contents.append(buffer, count);
  }
  if (std::ferror(input.get())) {
    *error = "Cannot read native public-library configuration " + path;
    return false;
  }
  std::istringstream lines(contents);
  std::string line;
  size_t number = 0;
  while (std::getline(lines, line)) {
    ++number;
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    std::istringstream words(line);
    std::vector<std::string> tokens;
    std::string token;
    while (words >> token) tokens.push_back(token);
    if (tokens.empty()) continue;
    bool valid = tokens.size() <= 3;
    int bitness = 0;
    for (size_t i = 1; valid && i < tokens.size(); ++i) {
      if (tokens[i] == "nopreload") {
        // Preloading is not needed for this availability snapshot. The pinned
        // parser accepts the qualifier more than once within its token limit.
        continue;
      } else if ((tokens[i] == "32" || tokens[i] == "64") && bitness == 0) {
        bitness = tokens[i] == "32" ? 32 : 64;
      } else {
        valid = false;
      }
    }
    const std::string& name = tokens.front();
    valid = valid && name != "." && name != ".." &&
        name.find_first_of("/\\:") == std::string::npos;
    // Both supported Android execution ABIs are ELF64. Match the pinned
    // parser's bitness filter before the extension's company-name policy.
    if (valid && bitness == 32) continue;
    if (!company.empty()) {
      valid = valid && name.compare(0, 3, "lib") == 0 && EndsWith(name, "." + company + ".so");
    }
    if (!valid) {
      *error = "Invalid native public-library declaration at " + path + ":" + std::to_string(number);
      return false;
    }
    const std::string library_path = directory + "/" + name;
    File library(std::fopen(library_path.c_str(), "rb"));
    if (library == nullptr) continue;
    struct stat st{};
    Elf64_Ehdr header{};
    if (fstat(fileno(library.get()), &st) != 0 || !S_ISREG(st.st_mode) ||
        std::fread(&header, 1, sizeof(header), library.get()) != sizeof(header) ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT ||
        header.e_type != ET_DYN) continue;
    elf::Architecture architecture;
    if (header.e_machine == EM_X86_64) architecture = elf::Architecture::kX86_64;
    else if (header.e_machine == EM_AARCH64) architecture = elf::Architecture::kAarch64;
    else continue;
    files->push_back({name, library_path, directory, visibility, architecture, vendor});
  }
  return true;
}

bool ReadExtensions(const std::string& root, std::vector<PublicLibraryFile>* files,
                    std::string* error) {
  const std::string etc = root + "/etc";
  std::unique_ptr<DIR, DirectoryCloser> directory(opendir(etc.c_str()));
  if (directory == nullptr) {
    if (errno == ENOENT || errno == ENOTDIR) return true;
    *error = "Cannot enumerate native public-library configuration " + etc + ": " + std::strerror(errno);
    return false;
  }
  const std::string prefix = "public.libraries-";
  const std::string suffix = ".txt";
  std::vector<std::string> configs;
  errno = 0;
  while (dirent* entry = readdir(directory.get())) {
    const std::string name(entry->d_name);
    if (name.compare(0, prefix.size(), prefix) == 0 && EndsWith(name, suffix)) {
      configs.push_back(name);
    }
  }
  if (errno != 0) {
    *error = "Cannot enumerate native public-library configuration " + etc + ": " + std::strerror(errno);
    return false;
  }
  std::sort(configs.begin(), configs.end());
  for (const std::string& name : configs) {
    const std::string company = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (company.empty()) {
      *error = "Empty company name in native public-library configuration " + etc + "/" + name;
      return false;
    }
    if (!ReadConfig(etc + "/" + name, root + "/lib64", company,
                    elf::ProviderVisibility::kDevicePublic, false, files, error)) return false;
  }
  return true;
}

}  // namespace

std::string SystemRoot() {
  const char* root = std::getenv("ANDROID_ROOT");
  return StripSlash(root != nullptr && *root != '\0' ? root : "/system");
}

bool ReadPublicLibraryFiles(const std::string& system_root,
                            std::vector<PublicLibraryFile>* files, std::string* error) {
  files->clear();
  const std::string system = StripSlash(system_root);
  const size_t slash = system.find_last_of('/');
  const std::string parent = slash == std::string::npos ? "." : system.substr(0, slash);
  const std::string vendor = parent + "/vendor";
  if (!ReadConfig(system + "/etc/public.libraries.txt", system + "/lib64", "",
                  elf::ProviderVisibility::kNdkPublic, false, files, error) ||
      !ReadConfig(vendor + "/etc/public.libraries.txt", vendor + "/lib64", "",
                  elf::ProviderVisibility::kDevicePublic, true, files, error) ||
      !ReadExtensions(system, files, error) ||
      !ReadExtensions(parent + "/system_ext", files, error) ||
      !ReadExtensions(parent + "/product", files, error)) {
    files->clear();
    return false;
  }
  // Keep the configuration search order for duplicate declarations of the
  // same SONAME and ABI. One catalog row cannot promise two different files.
  std::set<std::pair<std::string, elf::Architecture>> seen;
  files->erase(std::remove_if(files->begin(), files->end(), [&](const PublicLibraryFile& file) {
    return !seen.emplace(file.soname, file.architecture).second;
  }), files->end());
  return true;
}

}  // namespace gesher::nativeloader
