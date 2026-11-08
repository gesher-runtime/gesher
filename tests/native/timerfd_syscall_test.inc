// Linux x86-64 syscall entry regression for Cronet's timerfd-driven network loop.
// Built as an Android ELF library; elf_load_test calls gesher_selftest().
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

#if defined(__x86_64__)
_Static_assert(SYS_timerfd_create == 283 && SYS_timerfd_settime == 286 && SYS_timerfd_gettime == 287,
               "Unexpected Linux x86-64 syscall table");
#endif

static void test_timerfd_syscalls(void) {
    // This exact syscall/clock/flag combination triggered Cronet's fatal CHECK.
    int fd = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    check("Cronet create(CLOCK_MONOTONIC, NONBLOCK|CLOEXEC)", fd >= 0);
    if (fd < 0) return;
    uint64_t count = 0;
    errno = 0;
    check("unarmed nonblocking read returns EAGAIN", read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    struct itimerspec current, previous;
    memset(&current, 0x5a, sizeof(current));
    check("numeric gettime starts disarmed", syscall(SYS_timerfd_gettime, fd, &current) == 0
          && current.it_value.tv_sec == 0 && current.it_value.tv_nsec == 0);
    struct itimerspec arm = { .it_value = { .tv_sec = 0, .tv_nsec = 30000000 } };
    check("numeric settime arms one-shot timer", syscall(SYS_timerfd_settime, fd, 0, &arm, &previous) == 0);
    check("old timer state returned", previous.it_value.tv_sec == 0 && previous.it_value.tv_nsec == 0);
    struct pollfd pending = { .fd = fd, .events = POLLIN };
    check("armed timer becomes poll-readable", poll(&pending, 1, 1500) == 1 && (pending.revents & POLLIN));
    check("read consumes one expiration", read(fd, &count, sizeof(count)) == sizeof(count) && count == 1);
    errno = 0;
    check("consumed nonblocking read returns EAGAIN", read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    struct itimerspec disarm = {0};
    arm.it_value.tv_sec = 1; arm.it_value.tv_nsec = 0;
    check("numeric rearm works", syscall(SYS_timerfd_settime, fd, 0, &arm, 0) == 0);
    check("numeric disarm works", syscall(SYS_timerfd_settime, fd, 0, &disarm, &previous) == 0
          && previous.it_value.tv_sec >= 0 && previous.it_value.tv_nsec >= 0);
    check("disarmed gettime reports zero", syscall(SYS_timerfd_gettime, fd, &current) == 0
          && current.it_value.tv_sec == 0 && current.it_value.tv_nsec == 0);
    errno = 0;
    check("invalid create flags preserve EINVAL", syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0x40000000) == -1 && errno == EINVAL);
    errno = 0;
    check("invalid settime flags preserve EINVAL", syscall(SYS_timerfd_settime, fd, 0x4000, &arm, 0) == -1 && errno == EINVAL);
    errno = 0;
    check("null gettime destination preserves EFAULT", syscall(SYS_timerfd_gettime, fd, 0) == -1 && errno == EFAULT);
    check("timer descriptor closes", close(fd) == 0);
    errno = 0;
    check("unknown syscall stays ENOSYS", syscall(4000) == -1 && errno == ENOSYS);
}
