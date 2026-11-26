// One POSIX environment for host code and loaded Android libraries. UCRT owns
// only copied shadow values; it must never own a putenv caller's storage.
#pragma once

namespace gesher {
char*** EnvironmentSlot();
char* GetEnvironment(const char* name);
char* GetWindowsEnvironment(const char* name);
int SetEnvironment(const char* name, const char* value, int overwrite);
int PutEnvironment(char* assignment);
int UnsetEnvironment(const char* name);
int SetWideEnvironment(const wchar_t* name, const wchar_t* value);
int SetNativeEnvironment(const char* name, const char* value);
// The font layer retains its native case-alias/last-writer policy. Visit only
// when that key's selected owner is a putenv buffer; owned setenv/unsetenv keep
// the existing immutable font path. A null value is a borrowed-name tombstone.
// Returns 1 when visited, 0 for the existing font path, or -1 on init failure.
int VisitBorrowedFontEnvironment(const char* name, void (*visit)(const char*, void*), void* context);
// Calls visit for each current assignment under the same lock. The caller
// copies only when it actually needs an exec/spawn environment snapshot.
bool VisitEnvironment(void (*visit)(const char*, void*), void* context);

// Native shadow boundaries, called only by the process environment writer.
int WriteNativeEnvironment(const char* name, const char* value);
int WriteWideNativeEnvironment(const char* name, const char* value,
                               const wchar_t* wide_name, const wchar_t* wide_value);
bool PreserveCrtEnvironment(const char* name);
int RawPutEnvironment(const char* name, const char* value);
int RawPutWideEnvironment(const wchar_t* name, const wchar_t* value);
}  // namespace gesher
