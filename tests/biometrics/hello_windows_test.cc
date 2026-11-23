// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/hello_win.cc (ADR 0091, phase 2), run
// against the real UserConsentVerifier without showing any UI:
//  - availability is one of Windows' five values (a runner usually has no Hello);
//  - a verification for a process that owns no foreground window never starts:
//    kNotForeground, no prompt, no operation (SystemUI's foreground rule).
// tests/biometrics/hello_probe.cc covers verify/cancel by hand.

#include <windows.h>
#include <stdio.h>

#include "hello_win.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

int main() {
  HRESULT hr = E_FAIL;
  int availability = hello::Availability(&hr);
  printf("availability=%d hr=0x%08lx\n", availability, (unsigned long)hr);
  Check(availability >= 0 && availability <= 4 && SUCCEEDED(hr),
        "availability is a UserConsentVerifierAvailability value");
  Check(hello::Availability(&hr) == availability, "availability is stable across queries");

  const wchar_t message[] = L"must not be shown";
  // This console test owns no window, so it cannot be the foreground owner.
  hello::StartFailure failure = hello::StartFailure::kNone;
  hr = E_FAIL;
  hello::Verification* v = hello::Start(GetCurrentProcessId(), message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground && hr == S_OK,
        "a process without the foreground window gets kNotForeground and no prompt");
  if (v != nullptr) {
    hello::Cancel(v);
    hello::Await(v);
    hello::Release(v);
  }

  // PID 0 (System Idle) and a PID no process has: never an owner.
  failure = hello::StartFailure::kNone;
  v = hello::Start(0, message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground, "pid 0 is never an owner");
  failure = hello::StartFailure::kNone;
  v = hello::Start(0xFFFFFFF0u, message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground,
        "an unknown pid is never an owner");

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
