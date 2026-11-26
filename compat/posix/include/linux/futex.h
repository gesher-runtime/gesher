// gesher-base: <linux/futex.h>. syscall(SYS_futex, ...) is implemented on
// WaitOnAddress / WakeByAddress (compat/posix/src/syscall.cc).
#pragma once

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10

#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_WAIT_PRIVATE        (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE        (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE     (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_OP_PRIVATE     (FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

#define FUTEX_OP_SET          0
#define FUTEX_OP_ADD          1
#define FUTEX_OP_OR           2
#define FUTEX_OP_ANDN         3
#define FUTEX_OP_XOR          4
#define FUTEX_OP_OPARG_SHIFT  8

#define FUTEX_OP_CMP_EQ       0
#define FUTEX_OP_CMP_NE       1
#define FUTEX_OP_CMP_LT       2
#define FUTEX_OP_CMP_LE       3
#define FUTEX_OP_CMP_GT       4
#define FUTEX_OP_CMP_GE       5

#define FUTEX_OP(op, oparg, cmp, cmparg) \
    (((unsigned)(op) & 0xf) << 28 | ((unsigned)(cmp) & 0xf) << 24 | \
     ((unsigned)(oparg) & 0xfff) << 12 | ((unsigned)(cmparg) & 0xfff))
