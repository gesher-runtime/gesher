/* Shared LP64 raw syscall fixture. raw6 exposes the kernel's signed result. */
static void test_positioned_vectors(void) {
    int fd = (int)syscall(SYS_memfd_create, "positioned-vectors", 0u);
    check("positioned vectors create a real file", fd >= 0);
    if (fd < 0) return;
    char first[] = "ab", second[] = "CDE";
    struct iovec vector[2] = {{first, sizeof(first) - 1}, {second, sizeof(second) - 1}};
    char left[2] = {0}, right[3] = {0};
    struct iovec output[2] = {{left, sizeof(left)}, {right, sizeof(right)}};
    const long beyond_4gb = (long)(UINT64_C(1) << 32) + 1;
    const long unused_high = 0x12345678;
    check("positioned vectors seed data and set ordinary cursor", write(fd, "01234567", 8) == 8 &&
          lseek(fd, 3, SEEK_SET) == 3);
    errno = EDOM;
    check("raw pwritev uses the complete position and ignores the fifth register",
          raw6(SYS_pwritev, fd, (long)vector, 2, 1, unused_high, 0) == 5 && errno == EDOM);
    check("raw preadv scatters real bytes and ignores the fifth register",
          raw6(SYS_preadv, fd, (long)output, 2, 1, unused_high, 0) == 5 &&
          memcmp(left, "ab", sizeof(left)) == 0 && memcmp(right, "CDE", sizeof(right)) == 0);
    check("raw preadv preserves the full offset beyond 4 GiB",
          raw6(SYS_preadv, fd, (long)output, 2, beyond_4gb, 0, 0) == 0);
    check("positioned vectors leave the ordinary cursor alone", lseek(fd, 0, SEEK_CUR) == 3);
    errno = EDOM;
    check("raw negative offset is rejected before fd and leaves libc errno alone",
          raw6(SYS_preadv, -1, 0, 0, -1, 0, 0) == -EINVAL && errno == EDOM);
    check("raw positioned vectors import only the low count word",
          raw6(SYS_preadv, fd, (long)output, beyond_4gb, 1, 0, 0) == 2 &&
          raw6(SYS_pwritev, fd, (long)vector, beyond_4gb, 1, 0, 0) == 2);
    check("raw positioned vectors reject a count above IOV_MAX",
          raw6(SYS_preadv, fd, (long)output, 1025, 0, 0, 0) == -EINVAL &&
          raw6(SYS_pwritev, fd, (long)vector, 1025, 0, 0, 0) == -EINVAL);
    check("raw inaccessible iovec is EFAULT for either direction",
          raw6(SYS_preadv, fd, 1, 1, 0, 0, 0) == -EFAULT &&
          raw6(SYS_pwritev, fd, 1, 1, 0, 0, 0) == -EFAULT);
    check("raw empty vectors do not touch the supplied iovec",
          raw6(SYS_preadv, fd, 1, 0, 0, 0, 0) == 0 && raw6(SYS_pwritev, fd, 1, 0, 0, 0, 0) == 0);
    errno = 0;
    check("libc syscall preadv returns the raw result through errno",
          syscall(SYS_preadv, fd, output, 2, -1L, 0L) == -1 && errno == EINVAL);
    check("libc syscall pwritev dispatches the same ordinary file operation",
          syscall(SYS_pwritev, fd, vector, 2, 1L, 0L) == 5);
    close(fd);
}
