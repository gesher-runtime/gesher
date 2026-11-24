// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/hello_key_win.cc (ADR 0092), run against
// the real KeyCredentialManager without showing any UI:
//  - IsSupportedAsync answers (a runner usually has no Windows Hello);
//  - a key that does not exist is NotFound, for OpenAsync and for deleting it;
//  - creating a key or signing for a process that owns no foreground window never
//    starts: kNotForeground, no prompt.
// Creating and signing need a person: tests/biometrics/hello_key_probe.cc.

#include <windows.h>
#include <stdio.h>

#include <vector>

#include "hello_key_win.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

int main() {
  HRESULT hr = E_FAIL;
  int supported = hello_key::Supported(&hr);
  printf("supported=%d hr=0x%08lx\n", supported, (unsigned long)hr);
  Check((supported == 0 || supported == 1) && SUCCEEDED(hr), "IsSupportedAsync answers yes or no");
  Check(hello_key::Supported(&hr) == supported, "and the same when asked again");

  const wchar_t name[] = L"Gesher test: a key nobody created 7c1d6f0e";
  const size_t length = sizeof(name) / sizeof(name[0]) - 1;
  std::vector<uint8_t> bytes(3, 0x55);
  hr = E_FAIL;
  int status = hello_key::Open(name, length, &bytes, &hr);
  printf("open status=%d hr=0x%08lx\n", status, (unsigned long)hr);
  Check(status == 2 && bytes.empty(), "opening a key that does not exist: NotFound, no public key");
  status = hello_key::Delete(name, length, &hr);
  printf("delete status=%d hr=0x%08lx\n", status, (unsigned long)hr);
  Check(status == 2, "deleting a key that does not exist: NotFound");

  // This console test owns no window, so it cannot be the foreground owner.
  const uint8_t data[] = {1, 2, 3};
  hello_key::Operation* o = hello_key::StartCreate(GetCurrentProcessId(), name, length);
  bytes.assign(3, 0x55);
  status = hello_key::Await(o, &bytes);
  hello_key::Cancel(o);  // harmless on an operation that never started
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground && bytes.empty(),
        "creating a key without the foreground window: kNotForeground, no prompt");
  o = hello_key::StartSign(GetCurrentProcessId(), name, length, data, sizeof(data));
  status = hello_key::Await(o, &bytes);
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground && bytes.empty(),
        "signing without the foreground window: kNotForeground, no prompt");
  o = hello_key::StartSign(0xFFFFFFF0u, name, length, data, sizeof(data));
  status = hello_key::Await(o, &bytes);
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground, "an unknown pid is never an owner");
  Check(hello_key::Open(name, length, &bytes, &hr) == 2, "and no key was created by any of it");

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
