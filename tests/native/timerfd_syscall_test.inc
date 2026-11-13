// Shared Linux and Android x86-64/ARM64 timerfd syscall/ownership contracts.
// Built as an Android ELF library; elf_load_test calls gesher_selftest().
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#if !defined(GESHER_TIMERFD_HOST)
#include <sys/syscall.h>
#include <sys/timerfd.h>
#endif
#include <unistd.h>

#if defined(__x86_64__)
_Static_assert(SYS_timerfd_create == 283 && SYS_timerfd_settime == 286 && SYS_timerfd_gettime == 287,
               "Unexpected Linux x86-64 syscall table");
#endif
#if defined(__aarch64__)
_Static_assert(SYS_timerfd_create == 85 && SYS_timerfd_settime == 86 && SYS_timerfd_gettime == 87,
               "Unexpected Linux ARM64 syscall table");
#endif

static void test_timerfd_vector_write_errors(int fd) {
    uint64_t injected = 1;
    struct iovec zeros[] = { {0, 0}, {0, 0} };
    struct iovec payload = { &injected, sizeof(injected) };
    errno = 0;
    check("timer rejects empty writev", writev(fd, 0, 0) == -1 && errno == EINVAL);
    errno = 0;
    check("timer rejects zero-length writev", writev(fd, zeros, 1) == -1 && errno == EINVAL);
    errno = 0;
    check("timer rejects all-zero writev", writev(fd, zeros, 2) == -1 && errno == EINVAL);
    errno = 0;
    check("timer rejects payload writev", writev(fd, &payload, 1) == -1 && errno == EINVAL);
}

static void test_timerfd_syscalls(void) {
    // This exact syscall/clock/flag combination triggered Cronet's fatal CHECK.
    int fd = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    check("Cronet create(CLOCK_MONOTONIC, NONBLOCK|CLOEXEC)", fd >= 0);
    if (fd < 0) return;
    uint64_t count = 0;
    errno = 0;
    check("unarmed nonblocking read returns EAGAIN", read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    test_timerfd_vector_write_errors(fd);
    struct itimerspec current, previous;
    memset(&current, 0x5a, sizeof(current));
    check("numeric gettime starts disarmed", syscall(SYS_timerfd_gettime, fd, &current) == 0
          && current.it_value.tv_sec == 0 && current.it_value.tv_nsec == 0);
    struct itimerspec arm = { .it_interval = { .tv_sec = 0, .tv_nsec = 0 },
                             .it_value = { .tv_sec = 0, .tv_nsec = 30000000 } };
    check("numeric settime arms one-shot timer", syscall(SYS_timerfd_settime, fd, 0, &arm, &previous) == 0);
    check("old timer state returned", previous.it_value.tv_sec == 0 && previous.it_value.tv_nsec == 0);
    struct pollfd pending = { .fd = fd, .events = POLLIN, .revents = 0 };
    check("armed timer becomes poll-readable", poll(&pending, 1, 1500) == 1 && (pending.revents & POLLIN));
    check("read consumes one expiration", read(fd, &count, sizeof(count)) == sizeof(count) && count == 1);
    errno = 0;
    check("consumed nonblocking read returns EAGAIN", read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    struct itimerspec disarm;
    memset(&disarm, 0, sizeof(disarm));
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
#if !defined(GESHER_TIMERFD_HOST)
    errno = 0;
    check("unknown syscall stays ENOSYS", syscall(4000) == -1 && errno == ENOSYS);
#endif

    // A timer belongs to a shared open description, rather than its first fd.
    fd = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
    int alias = dup(fd);
    check("timer alias created", fd >= 0 && alias >= 0);
    if (fd < 0 || alias < 0) { if (fd >= 0) close(fd); return; }
    check("gettime through duplicated timer", syscall(SYS_timerfd_gettime, alias, &current) == 0);
    arm.it_interval.tv_sec = 2; arm.it_interval.tv_nsec = 0;
    arm.it_value.tv_sec = 10; arm.it_value.tv_nsec = 0;
    check("settime through duplicated timer", syscall(SYS_timerfd_settime, alias, 0, &arm, 0) == 0);
    check("original sees shared interval", syscall(SYS_timerfd_gettime, fd, &current) == 0 &&
            current.it_interval.tv_sec == 2 && current.it_value.tv_sec >= 8);
    close(fd);
    errno = 0;
    check("closed original timer is EBADF", syscall(SYS_timerfd_gettime, fd, &current) == -1 && errno == EBADF);
    check("alias survives original close", syscall(SYS_timerfd_gettime, alias, &current) == 0);
    int ordinary = open("/dev/null", O_RDONLY);
    check("ordinary fd for reuse", ordinary >= 0);
    if (ordinary >= 0) {
        check("ordinary descriptor replaces old timer number", dup2(ordinary, fd) == fd);
        errno = 0;
        check("reused non-timer gettime is EINVAL", syscall(SYS_timerfd_gettime, fd, &current) == -1 && errno == EINVAL);
        errno = 0;
        check("reused non-timer settime is EINVAL", syscall(SYS_timerfd_settime, fd, 0, &disarm, 0) == -1 && errno == EINVAL);
        if (ordinary != fd) close(ordinary);
        close(fd);
    }
    check("disarm through surviving alias", syscall(SYS_timerfd_settime, alias, 0, &disarm, 0) == 0);
    pending.fd = alias; pending.events = POLLIN | POLLOUT; pending.revents = 0;
    check("unarmed timer never advertises writable readiness", poll(&pending, 1, 0) == 0 && pending.revents == 0);
    errno = 0; count = 1;
    check("timer does not accept injected expirations", write(alias, &count, sizeof(count)) == -1 && errno == EINVAL);
    test_timerfd_vector_write_errors(alias);
    errno = 0;
    check("short timer read is EINVAL", read(alias, &count, 7) == -1 && errno == EINVAL);

    arm.it_interval.tv_sec = 0; arm.it_interval.tv_nsec = 0;
    arm.it_value.tv_sec = 0; arm.it_value.tv_nsec = 2000000;
    check("arm timer before unread disarm", syscall(SYS_timerfd_settime, alias, 0, &arm, 0) == 0);
    pending.events = POLLIN;
    check("unread timer expires", poll(&pending, 1, 1500) == 1 && (pending.revents & POLLIN));
    check("disarm resets unread readiness", syscall(SYS_timerfd_settime, alias, 0, &disarm, 0) == 0 &&
            poll(&pending, 1, 0) == 0);
    errno = 0;
    check("disarm resets unread count", read(alias, &count, sizeof(count)) == -1 && errno == EAGAIN);
    check("arm timer before unread rearm", syscall(SYS_timerfd_settime, alias, 0, &arm, 0) == 0);
    check("timer expires again", poll(&pending, 1, 1500) == 1 && (pending.revents & POLLIN));
    arm.it_value.tv_sec = 10; arm.it_value.tv_nsec = 0;
    check("rearm resets unread readiness", syscall(SYS_timerfd_settime, alias, 0, &arm, 0) == 0 &&
            poll(&pending, 1, 0) == 0);
    errno = 0;
    check("rearm resets unread count", read(alias, &count, sizeof(count)) == -1 && errno == EAGAIN);

    struct itimerspec invalid = arm;
    invalid.it_value.tv_sec = -1;
    errno = 0;
    check("negative value seconds rejected", syscall(SYS_timerfd_settime, alias, 0, &invalid, 0) == -1 && errno == EINVAL);
    invalid = arm; invalid.it_interval.tv_sec = -1;
    errno = 0;
    check("negative interval seconds rejected", syscall(SYS_timerfd_settime, alias, 0, &invalid, 0) == -1 && errno == EINVAL);
    check("invalid time leaves prior timer armed", syscall(SYS_timerfd_gettime, alias, &current) == 0 &&
            current.it_value.tv_sec >= 8);
    errno = 0;
    check("unsupported timer clock rejected", syscall(SYS_timerfd_create, CLOCK_MONOTONIC_RAW, TFD_NONBLOCK) == -1 && errno == EINVAL);
    check("irrelevant monotonic cancel flag accepted", syscall(SYS_timerfd_settime, alias, TFD_TIMER_CANCEL_ON_SET, &disarm, 0) == 0);
    struct itimerspec huge_interval = disarm;
    huge_interval.it_interval.tv_sec = INT64_MAX / 1000000000;
    check("large interval accepted", syscall(SYS_timerfd_settime, alias, 0, &huge_interval, 0) == 0);
    check("Linux ktime saturation threshold retained", syscall(SYS_timerfd_gettime, alias, &current) == 0 &&
            current.it_interval.tv_sec == INT64_MAX / 1000000000 &&
            current.it_interval.tv_nsec == INT64_MAX % 1000000000);

    int another = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
    check("second timer created", another >= 0);
    if (another >= 0) {
        check("dup2 replaces timer with shared timer", dup2(alias, another) == another);
        test_timerfd_vector_write_errors(another);
        arm.it_interval.tv_sec = 3;
        check("dup2 timer alias accepts setting", syscall(SYS_timerfd_settime, another, 0, &arm, 0) == 0);
        check("dup2 source sees alias interval", syscall(SYS_timerfd_gettime, alias, &current) == 0 && current.it_interval.tv_sec == 3);
        close(another);
    }
    close(alias);

    fd = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
    check("periodic timer created", fd >= 0);
    if (fd >= 0) {
        arm.it_value.tv_sec = 0; arm.it_value.tv_nsec = 2000000;
        arm.it_interval.tv_sec = 0; arm.it_interval.tv_nsec = 2000000;
        check("periodic timer armed", syscall(SYS_timerfd_settime, fd, 0, &arm, 0) == 0);
        usleep(30000);
        check("periodic read counts multiple expirations", read(fd, &count, sizeof(count)) == sizeof(count) && count >= 2);
        check("periodic next deadline remains within interval", syscall(SYS_timerfd_gettime, fd, &current) == 0 &&
                current.it_interval.tv_nsec == 2000000 && current.it_value.tv_sec == 0 &&
                current.it_value.tv_nsec > 0 && current.it_value.tv_nsec <= 2000000);
        close(fd);
    }
    fd = (int) syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
    check("blocking timer created", fd >= 0);
    if (fd >= 0) {
        arm.it_value.tv_sec = 0; arm.it_value.tv_nsec = 2000000;
        arm.it_interval.tv_sec = 0; arm.it_interval.tv_nsec = 0;
        check("blocking timer armed", syscall(SYS_timerfd_settime, fd, 0, &arm, 0) == 0);
        check("blocking read wakes on expiration", read(fd, &count, sizeof(count)) == sizeof(count) && count == 1);
        close(fd);
    }
}
