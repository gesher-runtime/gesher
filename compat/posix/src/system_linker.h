// gesher-base: Gesher's x86-64 ELF loader under Android's name for the
// dynamic linker, /system/bin/linker64 (system_linker.cc,
// docs/decisions/0096-elf-loader-as-system-linker64.md).
#pragma once

#include <string>

namespace gesher::system_linker {

// The name: on a 64-bit Android device the dynamic linker, mapped in every
// process.
inline constexpr char kPath[] = "/system/bin/linker64";

// The file's bytes: an ELF64 little-endian EM_X86_64 header (the machine
// Gesher's loader loads natively) naming itself as Gesher's ELF loader. It has
// no entry point and no code: run directly it fails to link, and exec of
// /system/bin/linker64 is handled by name (exec_spawn.cc).
std::string Image();

// The Windows path of the file on disk holding Image(), under
// $ANDROID_DATA/system/gesher-linker/<hash of the bytes>/linker64, written
// once (image directories unused for a week and dead processes' temporary
// files are pruned); empty when it cannot be written. Final
// (GetFinalPathNameByHandleW) form, without a \\?\ prefix.
const std::wstring& File();

// Whether the Linux path `path` names kPath: absolute, with empty and "."
// components dropped and ".." resolved, no trailing slash.
bool IsName(const char* path);

// For calls that would change the file or its name (open for writing or
// truncation, unlink, rename, link, symlink, chmod, chown, utimes, xattrs):
// -1 with errno EROFS when `path` IsName, as /system is a read-only file
// system on Android; 0 otherwise.
int RefuseChange(const char* path);

// Whether `windows` (a Windows path, with or without \\?\) is File(), so that
// /proc/self/maps, /proc/self/fd and realpath name it as kPath.
bool IsFile(const std::wstring& windows);

// Maps File() read-only for the rest of the process's life, once, as the
// dynamic linker is mapped in every Android process. /proc/self/maps calls it
// before it lists the mappings; nothing happens when File() is empty.
void EnsureMapped();

}  // namespace gesher::system_linker
