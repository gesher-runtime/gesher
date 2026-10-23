// Gesher: the part of libselinux's <selinux/android.h> servicemanager's
// Access.cpp names off Android, where it allows every lookup and
// registration without SELinux (docs/services.md).
#pragma once

#include <stdlib.h>

// Access::~Access frees its own context, which stays null here.
static inline void freecon(char* con) { free(con); }
