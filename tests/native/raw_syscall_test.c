// Raw Linux system calls and code made at run time in an x86-64 library, as
// packers and anti-tamper code use them (ADR 0084). Built by native-libs.yml
// as raw_syscall_test.so and, packed with UPX, as raw_syscall_upx.so, whose
// code the loader can only see once its initializer has unpacked it.
// elf_load_test calls gesher_selftest().
//
//  - `syscall` instructions (getpid, gettid, write/read on a pipe, mmap,
//    mprotect, munmap, uname, clock_gettime, an unknown number): the loader
//    turns each into a trap the bionic shim emulates;
//  - syscall(2) for numbers it formerly refused;
//  - ptrace(PTRACE_TRACEME) twice (0, then EPERM) and regcomp/regexec;
//  - code written into the library's own pages and made executable with
//    mprotect: a raw system call and a stack-guard read in it are rewritten
//    when it first runs.
#include <errno.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef GESHER_UPX_VARIANT
// UPX packs a shared library through its DT_INIT, which it runs once it has
// unpacked the code (-Wl,-init=gesher_upx_init: the packers' libraries have
// one), and only one worth compressing.
static int upx_init_ran;
void gesher_upx_init(void) { upx_init_ran = 1; }
int gesher_upx_init_ran(void) { return upx_init_ran; }
__attribute__((used)) static const unsigned char gesher_upx_filler[256 * 1024] = {1, 2, 3};
#endif

static int failures;
static void check(const char* name, int ok) {
    printf("raw_syscall: %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

static long raw0(long nr) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory");
    return r;
}
static long raw3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long raw6(long nr, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

// The kernel clobbers %rcx and %r11 and keeps everything else.
static int raw_keeps_registers(void) {
    long r, rbx = 0x1111, r12 = 0x2222, r13 = 0x3333;
    __asm__ volatile("mov $39, %%eax\n\tsyscall"
                     : "=a"(r), "+b"(rbx), "+r"(r12), "+r"(r13)
                     :
                     : "rcx", "r11", "memory");
    return r > 0 && rbx == 0x1111 && r12 == 0x2222 && r13 == 0x3333;
}

// Two pages of the library's own code, written at run time.
__asm__(".section .text.gesher_patch,\"ax\",@progbits\n"
        ".p2align 12\n"
        ".globl gesher_patch_area\n.hidden gesher_patch_area\n"
        "gesher_patch_area:\n"
        ".fill 8192,1,0xcc\n"
        ".text\n");
extern unsigned char gesher_patch_area[];

__attribute__((noinline)) static uintptr_t covered_guard(void) {
    uintptr_t guard;
    __asm__ volatile("movq %%fs:0x28,%0" : "=r"(guard));
    return guard;
}

static void check_patched_code(void) {
    unsigned char* area = gesher_patch_area;
    if (mprotect(area, 8192, PROT_READ | PROT_WRITE) != 0) {
        check("own code made writable", 0);
        return;
    }
    // Page 1: mov $39,%eax; syscall; ret  (getpid)
    static const unsigned char getpid_code[] = {0xb8, 0x27, 0, 0, 0, 0x0f, 0x05, 0xc3};
    // Page 2: mov %fs:0x28,%rax; ret  (the stack guard)
    static const unsigned char guard_code[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0, 0xc3};
    memcpy(area, getpid_code, sizeof(getpid_code));
    memcpy(area + 4096, guard_code, sizeof(guard_code));
    if (mprotect(area, 8192, PROT_READ | PROT_EXEC) != 0) {
        check("own code made executable again", 0);
        return;
    }
    long (*patched_getpid)(void) = (long (*)(void))area;
    uintptr_t (*patched_guard)(void) = (uintptr_t (*)(void))(area + 4096);
    check("raw syscall in code written at run time", patched_getpid() == getpid());
    check("stack guard in code written at run time", patched_guard() == covered_guard());
    check("code written at run time runs again", patched_getpid() == getpid());
}

int gesher_selftest(void) {
    errno = EDOM;
    check("raw success preserves libc errno", raw0(SYS_getpid) == getpid() && errno == EDOM);
    errno = EDOM;
    check("raw failure preserves libc errno",
          raw3(SYS_close, 12345, 0, 0) == -EBADF && errno == EDOM);
    check("raw getpid", raw0(SYS_getpid) == getpid());
    check("raw gettid", raw0(SYS_gettid) == gettid());
    check("raw syscall keeps the registers the kernel keeps", raw_keeps_registers());
    check("raw unknown number is -ENOSYS", raw0(500) == -ENOSYS);
    check("raw bad descriptor is -EBADF", raw3(SYS_close, 12345, 0, 0) == -EBADF);

    // eventfd(2) predates eventfd2(2) and takes just initval. Unused argument
    // registers must not be forwarded as libc eventfd's flags.
    long event = raw3(SYS_eventfd, 7, -1, 0);
    uint64_t event_value = 0;
    check("raw eventfd ignores its unused flags register", event >= 0);
    if (event >= 0) {
        check("raw eventfd retains initial value",
              read((int)event, &event_value, sizeof(event_value)) == sizeof(event_value) &&
                  event_value == 7);
        close((int)event);
    }
    uint64_t cpus[16] = {0};
    long affinity = raw3(SYS_sched_getaffinity, 0, sizeof(cpus), (long)&cpus);
    uint64_t any_cpu = 0;
    for (size_t i = 0; i < sizeof(cpus) / sizeof(cpus[0]); ++i) any_cpu |= cpus[i];
    check("raw sched_getaffinity returns mask bytes instead of libc zero",
          affinity > 0 && affinity <= (long)sizeof(cpus) && affinity % sizeof(uint64_t) == 0 &&
              any_cpu != 0);
    check("raw rt_sigprocmask validates kernel sigsetsize",
          raw6(SYS_rt_sigprocmask, 0, 0, 0, 0, 0, 0) == -EINVAL);

    int fds[2];
    if (pipe(fds) == 0) {
        char buf[8] = {0};
        check("raw write", raw3(SYS_write, fds[1], (long)"hello", 5) == 5);
        check("raw read", raw3(SYS_read, fds[0], (long)buf, sizeof(buf)) == 5 &&
                              memcmp(buf, "hello", 5) == 0);
        close(fds[0]);
        close(fds[1]);
    } else {
        check("pipe", 0);
    }

    long p = raw6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("raw mmap", p > 0);
    if (p > 0) {
        ((volatile char*)p)[4097] = 7;
        check("raw mprotect", raw3(SYS_mprotect, p, 4096, PROT_READ) == 0);
        check("raw munmap", raw3(SYS_munmap, p, 8192, 0) == 0);
    }
    long bad = raw6(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, 12345, 0);  // no such fd
    check("raw mmap failure is -errno", bad < 0 && bad > -4096);

    struct utsname uts, libc_uts;
    check("raw uname", raw3(SYS_uname, (long)&uts, 0, 0) == 0 && uname(&libc_uts) == 0 &&
                           strcmp(uts.sysname, libc_uts.sysname) == 0 &&
                           strcmp(uts.machine, "x86_64") == 0);
    struct timespec ts = {0, 0};
    check("raw clock_gettime", raw3(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts, 0) == 0 &&
                                   (ts.tv_sec != 0 || ts.tv_nsec != 0));

    check("syscall(2) getppid", syscall(SYS_getppid) == getppid());
    errno = EDOM;
    check("syscall(2) success preserves errno", syscall(SYS_getpid) == getpid() && errno == EDOM);
    errno = 0;
    check("syscall(2) failure sets errno", syscall(SYS_close, 12345) == -1 && errno == EBADF);

    // /proc/self/auxv, which UPX's unpacker reads through raw openat/read.
    long auxv = raw3(SYS_openat, -100, (long)"/proc/self/auxv", 0);
    unsigned long pairs[64][2];
    long got = auxv >= 0 ? raw3(SYS_read, auxv, (long)pairs, sizeof(pairs)) : -1;
    unsigned long pagesz = 0;
    for (long i = 0; got > 0 && i < got / 16 && pairs[i][0] != 0; ++i) {
        if (pairs[i][0] == 6) pagesz = pairs[i][1];
    }
    if (auxv >= 0) raw3(SYS_close, auxv, 0, 0);
    check("raw openat/read of /proc/self/auxv gives AT_PAGESZ", pagesz == 4096);

    check("ptrace(PTRACE_TRACEME) succeeds once", ptrace(PTRACE_TRACEME, 0, 0, 0) == 0);
    errno = 0;
    check("ptrace(PTRACE_TRACEME) again is EPERM",
          ptrace(PTRACE_TRACEME, 0, 0, 0) == -1 && errno == EPERM);

    regex_t re;
    regmatch_t m[3];
    check("regcomp extended", regcomp(&re, "([a-z]+)-([0-9]+)", REG_EXTENDED) == 0 &&
                                  re.re_nsub == 2);
    check("regexec submatches", regexec(&re, "x foo-42 y", 3, m, 0) == 0 && m[0].rm_so == 2 &&
                                    m[0].rm_eo == 8 && m[1].rm_so == 2 && m[1].rm_eo == 5 &&
                                    m[2].rm_so == 6 && m[2].rm_eo == 8);
    check("regexec no match", regexec(&re, "nothing", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    const char bounded[] = {'x', '4', '2', 'y'};
    check("regcomp bounded input", regcomp(&re, "[0-9]+", REG_EXTENDED) == 0);
    m[0].rm_so = 1;
    m[0].rm_eo = 3;
    check("REG_STARTEND accepts a non-terminated byte sequence",
          regexec(&re, bounded, 1, m, REG_STARTEND) == 0 &&
              m[0].rm_so == 1 && m[0].rm_eo == 3);
    regfree(&re);
    check("regcomp basic, icase", regcomp(&re, "^fr\\(i\\)da", REG_ICASE | REG_NOSUB) == 0 &&
                                      regexec(&re, "FRIDA-server", 0, NULL, 0) == 0);
    regfree(&re);
    char message[64];
    check("regcomp bad pattern", regcomp(&re, "(", REG_EXTENDED) != 0);
    check("REG_NEWLINE matches line anchors",
          regcomp(&re, "^x$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nx\nb", 0, NULL, 0) == 0 &&
              regexec(&re, "a\nx\nb", 0, NULL, REG_NOTBOL | REG_NOTEOL) == 0);
    regfree(&re);
    check("REG_NEWLINE matches trailing empty line with offset",
          regcomp(&re, "^$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\n", 1, m, 0) == 0 && m[0].rm_so == 2 && m[0].rm_eo == 2);
    regfree(&re);
    check("REG_NEWLINE excludes newline from dot and negated class",
          regcomp(&re, "a.b", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    check("REG_NEWLINE excludes newline from negated bracket",
          regcomp(&re, "a[^x]b", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 0, NULL, 0) == REG_NOMATCH &&
              regexec(&re, "ayb", 0, NULL, 0) == 0);
    regfree(&re);
    check("REG_NEWLINE positive space and cntrl classes match newline",
          regcomp(&re, "a[[:space:]]b", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    check("REG_NEWLINE positive cntrl class matches newline",
          regcomp(&re, "a[[:cntrl:]]b", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    m[0].rm_so = 2;
    m[0].rm_eo = 3;
    check("REG_NEWLINE REG_STARTEND + REG_NOTBOL honors preceding newline",
          regcomp(&re, "^x$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nx", 1, m, REG_STARTEND | REG_NOTBOL) == 0 &&
              m[0].rm_so == 2 && m[0].rm_eo == 3);
    regfree(&re);
    check("REG_NEWLINE internal EOL anchor matches before LF",
          regcomp(&re, "a$\nb", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    check("REG_NEWLINE internal BOL anchor matches after LF",
          regcomp(&re, "a\n^b", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    check("REG_NEWLINE both internal anchors match across LF",
          regcomp(&re, "^a$\n^b$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    check("REG_NEWLINE escaped literal LF matches across lines",
          regcomp(&re, "^a\\\nb$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "a\nb", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 3);
    regfree(&re);
    check("POSIX leftmost-longest submatch preference",
          regcomp(&re, "(foo|foobar)(bar)?", REG_EXTENDED) == 0 &&
              regexec(&re, "foobar", 3, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 6 &&
              m[1].rm_so == 0 && m[1].rm_eo == 6 && m[2].rm_so == -1 && m[2].rm_eo == -1);
    regfree(&re);
    // Android android-16.0.0_r4 bionic is the oracle for these results.
    check("top-level alternative uses compiled entry",
          regcomp(&re, "a|bb", REG_EXTENDED) == 0 && regexec(&re, "bb", 1, m, 0) == 0 &&
              m[0].rm_so == 0 && m[0].rm_eo == 2);
    regfree(&re);
    const char* nullable_patterns[] = {"a*b", "a?b", "a{0}b", "a{0,2}b"};
    for (size_t i = 0; i < sizeof(nullable_patterns) / sizeof(nullable_patterns[0]); ++i) {
        check("nullable leading expression uses compiled entry",
              regcomp(&re, nullable_patterns[i], REG_EXTENDED) == 0 &&
                  regexec(&re, "b", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 1);
        regfree(&re);
    }
    check("BRE backreference executor uses compiled entry",
          regcomp(&re, "a*\\(b\\)\\1", 0) == 0 && regexec(&re, "bb", 1, m, 0) == 0 &&
              m[0].rm_so == 0 && m[0].rm_eo == 2);
    regfree(&re);
    m[0].rm_so = 11;
    m[0].rm_eo = 12;
    check("REG_NOSUB keeps internal backreferences without public output",
          regcomp(&re, "\\(ab\\)\\1", REG_NOSUB) == 0 && re.re_nsub == 1 &&
              regexec(&re, "abab", 1, m, 0) == 0 &&
              regexec(&re, "abac", 0, NULL, 0) == REG_NOMATCH &&
              m[0].rm_so == 11 && m[0].rm_eo == 12);
    regfree(&re);
    check("REG_ICASE keeps AOSP byte-exact backreferences",
          regcomp(&re, "\\(a\\)\\1", REG_ICASE) == 0 &&
              regexec(&re, "aA", 0, NULL, 0) == REG_NOMATCH &&
              regexec(&re, "AA", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 2);
    regfree(&re);
    check("backreference to an open outer group reports REG_ESUBREG",
          regcomp(&re, "\\(\\(a\\)\\1\\)", 0) == REG_ESUBREG);
    check("AOSP rejects empty alternatives",
          regcomp(&re, "(foo|foobar)(bar|)", REG_EXTENDED) == REG_EMPTY);
    check("AOSP rejects unsupported alphabetic escape",
          regcomp(&re, "\\d", REG_EXTENDED) == REG_EESCAPE);
    check("AOSP named collating element matches literal LF",
          regcomp(&re, "[[.newline.]]", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "x\ny", 1, m, 0) == 0 && m[0].rm_so == 1 && m[0].rm_eo == 2);
    regfree(&re);
    check("AOSP bracket word boundaries match",
          regcomp(&re, "[[:<:]]a[[:>:]]", REG_EXTENDED) == 0 &&
              regexec(&re, " a!", 1, m, 0) == 0 && m[0].rm_so == 1 && m[0].rm_eo == 2);
    regfree(&re);
    check("AOSP rejects repeated range", regcomp(&re, "[a-c-e]", REG_EXTENDED) == REG_ERANGE);
    check("AOSP repeated alternation picks greedy final capture",
          regcomp(&re, "(a|aa)*", REG_EXTENDED) == 0 && regexec(&re, "aaa", 3, m, 0) == 0 &&
              m[0].rm_so == 0 && m[0].rm_eo == 3 && m[1].rm_so == 2 && m[1].rm_eo == 3);
    regfree(&re);
    check("AOSP reversed repeated alternation picks greedy final capture",
          regcomp(&re, "(aa|a)*", REG_EXTENDED) == 0 && regexec(&re, "aaa", 3, m, 0) == 0 &&
              m[0].rm_so == 0 && m[0].rm_eo == 3 && m[1].rm_so == 2 && m[1].rm_eo == 3);
    regfree(&re);
    check("AOSP nested star empty and trailing empty captures",
          regcomp(&re, "(a*)*", REG_EXTENDED) == 0 && regexec(&re, "", 2, m, 0) == 0 &&
              m[1].rm_so == 0 && m[1].rm_eo == 0 && regexec(&re, "a", 2, m, 0) == 0 &&
              m[1].rm_so == 1 && m[1].rm_eo == 1);
    regfree(&re);
    check("AOSP nested BRE star with backref settles group 1 at trailing empty pass",
          regcomp(&re, "\\(a*\\)*\\1", 0) == 0 && regexec(&re, "aa", 2, m, 0) == 0 &&
              m[0].rm_so == 0 && m[0].rm_eo == 2 && m[1].rm_so == 2 && m[1].rm_eo == 2);
    regfree(&re);
    check("AOSP nested plus dissects only final iteration",
          regcomp(&re, "(a(b)|a)+", REG_EXTENDED) == 0 && regexec(&re, "aba", 3, m, 0) == 0 &&
              m[1].rm_so == 2 && m[1].rm_eo == 3 && m[2].rm_so == -1 && m[2].rm_eo == -1);
    regfree(&re);
    check("REG_NEWLINE extended accepts escaped ordinary slash and escaped bracket",
          regcomp(&re, "^\\|[0-9]{2}\\/[0-9]{5}\\^$", REG_EXTENDED | REG_NEWLINE) == 0 &&
              regexec(&re, "|11/11111^", 0, NULL, 0) == 0 &&
              regexec(&re, "|11x11111^", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    check("escaped bracket stays literal with escaped slash",
          regcomp(&re, "a\\[b\\/c\\]", REG_EXTENDED) == 0 &&
              regexec(&re, "a[b/c]", 0, NULL, 0) == 0 &&
              regexec(&re, "ab", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    // Pinned Bionic strip behavior found by the independent PR60 review.
    const char* word_starts[] = {"\\<a", "[[:<:]]a"};
    for (size_t i = 0; i < sizeof(word_starts) / sizeof(word_starts[0]); ++i) {
        check("slice word boundary compile", regcomp(&re, word_starts[i], REG_EXTENDED) == 0);
        m[0] = (regmatch_t){1, 2};
        check("slice begins a word unless NOTBOL", regexec(&re, "aa", 1, m, REG_STARTEND) == 0 &&
              m[0].rm_so == 1 && m[0].rm_eo == 2);
        m[0] = (regmatch_t){1, 2};
        check("NOTBOL keeps original preceding byte", regexec(&re, "aa", 1, m, REG_STARTEND | REG_NOTBOL) == REG_NOMATCH);
        regfree(&re);
    }
    const char* word_repeats[] = {"\\<*", "\\>*"};
    for (int flags = 0; flags <= REG_EXTENDED; ++flags) {
        for (size_t i = 0; i < sizeof(word_repeats) / sizeof(word_repeats[0]); ++i) {
            check("escaped boundary repetition", regcomp(&re, word_repeats[i], flags) == 0 &&
                  regexec(&re, "a", 1, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 0);
            regfree(&re);
        }
    }
    check("backreference prefilter keeps captured anchor", regcomp(&re, "\\(^a\\)\\1", 0) == 0 &&
          regexec(&re, "aa", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    check("bounded backreference strip selection", regcomp(&re, "\\(a\\)\\1\\{0,2\\}", 0) == 0 &&
          regexec(&re, "aaa", 2, m, 0) == 0 && m[0].rm_eo == 2 && m[1].rm_eo == 1);
    regfree(&re);
    check("backreference after bounded capture", regcomp(&re, "\\(a\\)\\{0,2\\}\\1", 0) == 0 &&
          regexec(&re, "a", 2, m, 0) == 0 && m[0].rm_so == 0 && m[0].rm_eo == 1 && m[1].rm_eo == 1);
    regfree(&re);

    check("regerror", regerror(REG_NOMATCH, &re, message, sizeof(message)) > 1 &&
                          message[0] != '\0');

    check_patched_code();
#ifdef GESHER_UPX_VARIANT
    check("DT_INIT ran after unpacking", gesher_upx_init_ran());
#endif

    if (failures == 0) printf("raw_syscall: all passed\n");
    fflush(stdout);
    return failures;
}
