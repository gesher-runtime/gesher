// Shared native-font paths for NDK metadata and ordinary POSIX file access.
#pragma once
#include <string>
namespace gesher {
struct NativeFontFiles {
  std::string xml;
  std::string directory;
};
// Explicit ANDROID_ROOT is authoritative. Without it, use the install's
// read-only fonts directory next to the actual executable, independently of cwd.
NativeFontFiles SystemFontFiles();
// Apply the same POSIX path model used by open/stat, including /mnt/<drive>.
std::wstring NativeFontWindowsPath(const std::string& path);
}  // namespace gesher
