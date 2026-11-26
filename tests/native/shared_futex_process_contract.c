// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// Each participant opens and maps the file independently. The control fields
// establish that the mapping is shared even if its futex queue is process-local.
struct Shared {
    _Atomic uint32_t word;
    _Atomic uint32_t other_word;
    _Atomic uint64_t waiter_address;
    _Atomic uint32_t waiter_pid;
};

static const unsigned kMask = 0x4;
static const unsigned kWrongMask = 0x8;

static long futex(_Atomic uint32_t* word, int operation, int value,
                  const struct timespec* time, unsigned mask) {
    return syscall(SYS_futex, word, operation, value, time, NULL, mask);
}

static int64_t elapsed_ms(const struct timespec* start) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return INT64_MAX;
    return ((int64_t)now.tv_sec - start->tv_sec) * 1000 +
           ((int64_t)now.tv_nsec - start->tv_nsec) / 1000000;
}

static int setup(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { perror("create suite file"); return 2; }
    int result = ftruncate(fd, sizeof(struct Shared));
    if (result != 0) perror("size suite file");
    close(fd);
    if (result != 0) { unlink(path); return 2; }
    puts("shared-futex setup PASS");
    return 0;
}

static struct Shared* open_map(const char* path, int* fd) {
    *fd = open(path, O_RDWR | O_CLOEXEC);
    if (*fd < 0) { perror("open suite file"); return MAP_FAILED; }
    struct stat info;
    if (fstat(*fd, &info) != 0 || info.st_size != (off_t)sizeof(struct Shared)) {
        fprintf(stderr, "suite file must be initialized with the setup command\n");
        close(*fd);
        return MAP_FAILED;
    }
    struct Shared* view = mmap(NULL, sizeof(*view), PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (view == MAP_FAILED) { perror("map suite file"); close(*fd); }
    return view;
}

static int wait_for_futex(struct Shared* view) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) { perror("clock"); return 2; }
    deadline.tv_sec += 7;
    atomic_store_explicit(&view->waiter_address, (uintptr_t)&view->word, memory_order_relaxed);
    atomic_store_explicit(&view->waiter_pid, (uint32_t)getpid(), memory_order_release);
    puts("shared-futex waiter READY");
    fflush(stdout);

    long result;
    do {
        result = futex(&view->word, FUTEX_WAIT_BITSET, 0, &deadline, kMask);
    } while (result == -1 && errno == EINTR);
    if (result != 0) { perror("shared-futex wait did not receive wake"); return 1; }
    puts("shared-futex waiter PASS");
    return 0;
}

static int wake_futex(struct Shared* view, int fd) {
    const struct timespec poll_delay = {0, 1000000};
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) { perror("clock"); return 2; }
    uint32_t waiter_pid;
    while ((waiter_pid = atomic_load_explicit(&view->waiter_pid, memory_order_acquire)) == 0) {
        if (elapsed_ms(&start) >= 3000) {
            fputs("shared-futex: no independent waiter in the shared mapping\n", stderr);
            return 2;
        }
        nanosleep(&poll_delay, NULL);
    }
    const uintptr_t waiter_address = atomic_load_explicit(&view->waiter_address, memory_order_relaxed);
    if (waiter_pid == (uint32_t)getpid() || waiter_address == 0) {
        fputs("shared-futex: waiter identity was invalid\n", stderr);
        return 2;
    }
    struct Shared* alias = MAP_FAILED;
    if (waiter_address == (uintptr_t)&view->word) {
        // Keep the first mapping occupied so the second mapping has a different VA.
        alias = mmap(NULL, sizeof(*alias), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (alias == MAP_FAILED) { perror("map alias at a distinct address"); return 2; }
        view = alias;
    }

    int verdict = 1;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock");
        verdict = 2;
    } else {
        do {
            if (futex(&view->word, FUTEX_WAKE_BITSET, 1, NULL, kWrongMask) != 0 ||
                futex(&view->other_word, FUTEX_WAKE_BITSET, 1, NULL, kMask) != 0 ||
                futex(&view->word, FUTEX_WAKE_PRIVATE, 1, NULL, 0) != 0) {
                fputs("shared-futex: wrong mask, offset or private key yielded a wake or an error\n", stderr);
                break;
            }
            const long result = futex(&view->word, FUTEX_WAKE_BITSET, 1, NULL, kMask);
            if (result == 1) {
                printf("shared-futex waker PASS (distinct pid and address: 0x%" PRIxPTR " vs %p)\n",
                       waiter_address, (void*)&view->word);
                verdict = 0;
                break;
            }
            if (result < 0) { perror("shared-futex wake"); break; }
            if (result > 1) { fputs("shared-futex woke more than one waiter\n", stderr); break; }
            nanosleep(&poll_delay, NULL);
        } while (elapsed_ms(&start) < 1500);
        if (verdict != 0) fputs("shared-futex: no matching cross-process wake observed\n", stderr);
    }
    if (alias != MAP_FAILED) munmap(alias, sizeof(*alias));
    return verdict;
}

int main(int argc, char** argv) {
    if (argc != 3 || (strcmp(argv[1], "setup") != 0 && strcmp(argv[1], "waiter") != 0 &&
                      strcmp(argv[1], "waker") != 0)) {
        fprintf(stderr, "usage: %s setup|waiter|waker FILE\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "setup") == 0) return setup(argv[2]);

    int fd;
    struct Shared* view = open_map(argv[2], &fd);
    if (view == MAP_FAILED) return 2;
    int result = strcmp(argv[1], "waiter") == 0 ? wait_for_futex(view) : wake_futex(view, fd);
    munmap(view, sizeof(*view));
    close(fd);
    return result;
}
