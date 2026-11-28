// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Used with the native Linux/Android syscall and the Windows component syscall.
static void CheckWakeOpWords(void) {
  int first = 101;
  int target = 7;
  struct Operation {
    unsigned encoded;
    int expected;
  };
  const struct Operation operations[] = {
    {FUTEX_OP(FUTEX_OP_SET, -7, FUTEX_OP_CMP_EQ, 7), -7},
    {FUTEX_OP(FUTEX_OP_ADD, -2, FUTEX_OP_CMP_NE, 0), -9},
    {FUTEX_OP(FUTEX_OP_SET, 12, FUTEX_OP_CMP_LT, 0), 12},
    {FUTEX_OP(FUTEX_OP_OR, 2, FUTEX_OP_CMP_GE, -1), 14},
    {FUTEX_OP(FUTEX_OP_ANDN, 6, FUTEX_OP_CMP_LE, 14), 8},
    {FUTEX_OP(FUTEX_OP_XOR, 3, FUTEX_OP_CMP_GT, -2), 11},
    {FUTEX_OP(((unsigned)FUTEX_OP_OR | FUTEX_OP_OPARG_SHIFT), 37, FUTEX_OP_CMP_EQ, 11), 43},
  };
  for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
    Require(WakeOperation(&first, &target, FUTEX_WAKE_OP_PRIVATE, 1, 1,
                          operations[i].encoded) == 0 &&
            target == operations[i].expected && first == 101, "atomic target update");
  }
  Require(WakeOperation(&first, &target, FUTEX_WAKE_OP_PRIVATE, 1, 1,
                        FUTEX_OP(6, 2, FUTEX_OP_CMP_EQ, 43)) == -1 &&
          errno == ENOSYS && target == 43, "unknown operation leaves word unchanged");
  Require(WakeOperation(&first, &target, FUTEX_WAKE_OP_PRIVATE, 1, 1,
                        FUTEX_OP(FUTEX_OP_ADD, 2, 7, 0)) == -1 &&
          errno == ENOSYS && target == 45, "unknown comparison keeps update without waking");
  Require(WakeOperation(&first, (int*)((unsigned char*)&target + 1), FUTEX_WAKE_OP_PRIVATE, 1, 1,
                        FUTEX_OP(FUTEX_OP_SET, 7, FUTEX_OP_CMP_EQ, 45)) == -1 &&
          errno == EINVAL && target == 45, "unaligned target rejected before update");
  Require(WakeOperation(&first, NULL, FUTEX_WAKE_OP_PRIVATE, 1, 1,
                        FUTEX_OP(FUTEX_OP_SET, 7, FUTEX_OP_CMP_EQ, 45)) == -1 &&
          errno == EFAULT, "unmapped target returns EFAULT");
}
