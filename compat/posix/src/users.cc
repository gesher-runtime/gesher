// gesher-base: <pwd.h> and <grp.h>. The process is Android's first app, uid
// and gid 10000 (see getuid), which Android names "u0_a0"; there are no
// other users or groups.
#include <errno.h>
#include <stdint.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr const char* kName = "u0_a0";
constexpr const char* kHome = "/data";
constexpr const char* kShell = "/system/bin/sh";

// Copies `s` into buf at *used; nullptr if it does not fit.
char* Put(const char* s, char* buf, size_t buflen, size_t* used) {
  size_t n = strlen(s) + 1;
  if (*used + n > buflen) return nullptr;
  char* out = buf + *used;
  memcpy(out, s, n);
  *used += n;
  return out;
}

int FillPasswd(struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
  *result = nullptr;
  size_t used = 0;
  pwd->pw_name = Put(kName, buf, buflen, &used);
  pwd->pw_passwd = Put("", buf, buflen, &used);
  pwd->pw_gecos = Put("", buf, buflen, &used);
  pwd->pw_dir = Put(kHome, buf, buflen, &used);
  pwd->pw_shell = Put(kShell, buf, buflen, &used);
  if (pwd->pw_name == nullptr || pwd->pw_passwd == nullptr || pwd->pw_gecos == nullptr ||
      pwd->pw_dir == nullptr || pwd->pw_shell == nullptr) {
    return ERANGE;
  }
  pwd->pw_uid = getuid();
  pwd->pw_gid = getgid();
  *result = pwd;
  return 0;
}

int FillGroup(struct group* grp, char* buf, size_t buflen, struct group** result) {
  *result = nullptr;
  // The member list, a single null pointer, at the first aligned spot in buf.
  size_t pad = (alignof(char*) - reinterpret_cast<uintptr_t>(buf) % alignof(char*)) %
               alignof(char*);
  size_t used = pad + sizeof(char*);
  if (buflen < used) return ERANGE;
  char** members = reinterpret_cast<char**>(buf + pad);
  members[0] = nullptr;
  grp->gr_name = Put(kName, buf, buflen, &used);
  grp->gr_passwd = Put("", buf, buflen, &used);
  if (grp->gr_name == nullptr || grp->gr_passwd == nullptr) return ERANGE;
  grp->gr_gid = getgid();
  grp->gr_mem = members;
  *result = grp;
  return 0;
}

thread_local struct passwd passwd_entry;
thread_local char passwd_buffer[128];
thread_local struct group group_entry;
alignas(char*) thread_local char group_buffer[128];

}  // namespace

extern "C" {

// The _r versions return 0 with *result == nullptr when there is no match.
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
  if (uid != getuid()) {
    *result = nullptr;
    return 0;
  }
  return FillPasswd(pwd, buf, buflen, result);
}

int getpwnam_r(const char* name, struct passwd* pwd, char* buf, size_t buflen,
               struct passwd** result) {
  if (name == nullptr || strcmp(name, kName) != 0) {
    *result = nullptr;
    return 0;
  }
  return FillPasswd(pwd, buf, buflen, result);
}

struct passwd* getpwuid(uid_t uid) {
  struct passwd* result;
  int error = getpwuid_r(uid, &passwd_entry, passwd_buffer, sizeof(passwd_buffer), &result);
  if (error != 0) errno = error;
  return result;
}

struct passwd* getpwnam(const char* name) {
  struct passwd* result;
  int error = getpwnam_r(name, &passwd_entry, passwd_buffer, sizeof(passwd_buffer), &result);
  if (error != 0) errno = error;
  return result;
}

int getgrgid_r(gid_t gid, struct group* grp, char* buf, size_t buflen, struct group** result) {
  if (gid != getgid()) {
    *result = nullptr;
    return 0;
  }
  return FillGroup(grp, buf, buflen, result);
}

int getgrnam_r(const char* name, struct group* grp, char* buf, size_t buflen,
               struct group** result) {
  if (name == nullptr || strcmp(name, kName) != 0) {
    *result = nullptr;
    return 0;
  }
  return FillGroup(grp, buf, buflen, result);
}

struct group* getgrgid(gid_t gid) {
  struct group* result;
  int error = getgrgid_r(gid, &group_entry, group_buffer, sizeof(group_buffer), &result);
  if (error != 0) errno = error;
  return result;
}

struct group* getgrnam(const char* name) {
  struct group* result;
  int error = getgrnam_r(name, &group_entry, group_buffer, sizeof(group_buffer), &result);
  if (error != 0) errno = error;
  return result;
}

}  // extern "C"
