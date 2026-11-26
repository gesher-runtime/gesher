#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void check(const char* name, int success) {
    ++checks;
    if (!success) {
        ++failures;
        printf("FAIL %s (errno=%d)\n", name, errno);
    }
}

static void verify_capacity(int read_end, int write_end) {
    int capacity = fcntl(write_end, F_GETPIPE_SZ);
    check("capacity is available from the writer", capacity > 0);
    check("reader observes the same capacity", fcntl(read_end, F_GETPIPE_SZ) == capacity && capacity > 0);
    const int writer = dup(write_end);
    check("writer alias shares capacity", writer >= 0 && fcntl(writer, F_GETPIPE_SZ) == capacity && capacity > 0);
    char block[4096] = {0};
    int used = 0;
    ssize_t written;
    do {
        written = write(write_end, block, sizeof(block));
        if (written > 0) used += (int)written;
    } while (written == sizeof(block) && used < 16 * 1024 * 1024);
    check("the reported capacity matches admitted bytes", written == -1 && errno == EAGAIN &&
          capacity > 0 && used == capacity);
    check("capacity does not shrink when filled", fcntl(write_end, F_GETPIPE_SZ) == capacity && capacity > 0);
    check("drain a block", read(read_end, block, sizeof(block)) == sizeof(block));
    check("capacity does not change when drained", fcntl(read_end, F_GETPIPE_SZ) == capacity && capacity > 0);
    check("close the reader", close(read_end) == 0);
    check("the remaining writer retains capacity", fcntl(write_end, F_GETPIPE_SZ) == capacity && capacity > 0);
    if (writer >= 0) close(writer);
    close(write_end);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    int fd[2];
    int made = pipe2(fd, O_NONBLOCK | O_CLOEXEC);
    check("create anonymous pipe", made == 0);
    if (made == 0) {
        puts("anonymous pipe");
        verify_capacity(fd[0], fd[1]);
    }

    made = mkfifo(argv[1], 0600);
    check("create named FIFO", made == 0);
    if (made == 0) {
        fd[0] = open(argv[1], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        fd[1] = fd[0] >= 0 ? open(argv[1], O_WRONLY | O_NONBLOCK | O_CLOEXEC) : -1;
        check("open FIFO endpoints", fd[0] >= 0 && fd[1] >= 0);
        if (fd[0] >= 0 && fd[1] >= 0) {
            puts("named FIFO");
            verify_capacity(fd[0], fd[1]);
        } else if (fd[0] >= 0) {
            close(fd[0]);
        }
        unlink(argv[1]);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    errno = 0;
    check("non-pipe socket is rejected", sock >= 0 && fcntl(sock, F_GETPIPE_SZ) == -1 && errno == EBADF);
    if (sock >= 0) close(sock);
    errno = 0;
    check("invalid descriptor is rejected", fcntl(-1, F_GETPIPE_SZ) == -1 && errno == EBADF);
    printf("pipe capacity: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
