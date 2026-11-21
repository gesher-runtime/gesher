// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "bionic_signal_range_contracts.h"

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  return BionicSignalRangeContracts() == 0 ? 0 : 1;
}
