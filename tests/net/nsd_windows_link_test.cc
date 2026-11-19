// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Link regression: retain the real guest POSIX entries alongside the complete
// Windows NSD transport. No socket, packet or JNI operation is executed here.
#include <sys/select.h>
#include <sys/socket.h>

namespace {
decltype(&::bind) volatile guest_bind = &::bind;
decltype(&::sendto) volatile guest_sendto = &::sendto;
decltype(&::setsockopt) volatile guest_setsockopt = &::setsockopt;
decltype(&::select) volatile guest_select = &::select;
}

int main() {
  return guest_bind && guest_sendto && guest_setsockopt && guest_select ? 0 : 1;
}
