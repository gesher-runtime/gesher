// gesher-base: <pwd.h>. There is one user: the app's uid (see getuid),
// named "u0_a0" as on Android, with home /data (src/users.cc).
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
  char* pw_name;
  char* pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  char* pw_gecos;
  char* pw_dir;
  char* pw_shell;
};

struct passwd* getpwnam(const char* name);
struct passwd* getpwuid(uid_t uid);
int getpwnam_r(const char* name, struct passwd* pwd, char* buf, size_t buflen,
               struct passwd** result);
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result);

#ifdef __cplusplus
}
#endif
