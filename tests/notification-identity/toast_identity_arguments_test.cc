/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include "../../compat/devsvc/toast_identity.h"
#include <cstdlib>
#include <cstdio>

extern "C" int toast_identity_argument_checks() {
  using gesher::devsvc::ValidToastIdentityArguments;
  using V = std::u16string_view;
  V valid[] = {u"Gesher.App.fixture", u"7|hash", u"android", u"n|expected-owner"};
  if (!ValidToastIdentityArguments(valid[0], valid[1], valid[2], valid[3])) std::abort();
  for (int at = 0; at < 4; ++at) {
    V values[] = {valid[0], valid[1], valid[2], valid[3]};
    values[at] = V(u"prefix\0different-owner", 22);
    if (ValidToastIdentityArguments(values[0], values[1], values[2], values[3])) {
      std::fprintf(stderr, "Embedded NUL crossed the conditional-removal owner boundary\n");
      std::abort();
    }
  }
  return 5;
}
