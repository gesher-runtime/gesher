// Mixed unwind coverage, as in Android libraries that omit unwind tables in
// part of their code. These assembly functions intentionally have no CFI/FDE.
// A normal C function retains an FDE, so the object has partial unwind metadata.
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

extern uintptr_t gap_guard_rax(void);
extern uintptr_t gap_guard_rcx(void);
extern uintptr_t gap_guard_r8(void);
extern int gap_guard_preserves_carry(void);
extern uintptr_t gap_guard_hidden(void);
extern uintptr_t gap_indirect_guard(void);
extern uintptr_t gap_indirect_carry(void);
extern uintptr_t gap_indirect_thread_pointer(void);
extern const volatile unsigned char gap_fs_like_table[];
static uintptr_t (*volatile indirect_guard)(void) = gap_indirect_guard;
static uintptr_t (*volatile indirect_carry)(void) = gap_indirect_carry;
static uintptr_t (*volatile indirect_thread_pointer)(void) = gap_indirect_thread_pointer;

__asm__(
    ".text\n"
    ".global gap_guard_rax\n.type gap_guard_rax,@function\n"
    "gap_guard_rax:\nmovq %fs:0x28,%rax\nret\n"
    ".size gap_guard_rax,.-gap_guard_rax\n"
    ".global gap_guard_rcx\n.type gap_guard_rcx,@function\n"
    "gap_guard_rcx:\nmovq %fs:0x28,%rcx\nmovq %rcx,%rax\nret\n"
    ".size gap_guard_rcx,.-gap_guard_rcx\n"
    ".global gap_guard_r8\n.type gap_guard_r8,@function\n"
    "gap_guard_r8:\nmovq %fs:0x28,%r8\nmovq %r8,%rax\nret\n"
    ".size gap_guard_r8,.-gap_guard_r8\n"
    ".global gap_guard_preserves_carry\n.type gap_guard_preserves_carry,@function\n"
    "gap_guard_preserves_carry:\nstc\nmovq %fs:0x28,%rdx\nsetc %al\nmovzbl %al,%eax\nret\n"
    ".size gap_guard_preserves_carry,.-gap_guard_preserves_carry\n"
    ".global gap_guard_hidden\n.hidden gap_guard_hidden\n.type gap_guard_hidden,@function\n"
    "gap_guard_hidden:\nmovq %fs:0x28,%r10\nmovq %r10,%rax\nret\n"
    ".size gap_guard_hidden,.-gap_guard_hidden\n"
    ".global gap_indirect_guard\n.hidden gap_indirect_guard\n.type gap_indirect_guard,@function\n"
    "gap_indirect_guard:\nmovq %fs:0x28,%rax\nret\n"
    ".size gap_indirect_guard,.-gap_indirect_guard\n"
    ".global gap_indirect_carry\n.hidden gap_indirect_carry\n.type gap_indirect_carry,@function\n"
    "gap_indirect_carry:\nstc\nmovq %fs:0x28,%r10\nsetc %al\nmovzbl %al,%eax\nret\n"
    ".size gap_indirect_carry,.-gap_indirect_carry\n"
    ".global gap_indirect_thread_pointer\n.hidden gap_indirect_thread_pointer\n.type gap_indirect_thread_pointer,@function\n"
    "gap_indirect_thread_pointer:\nmovq %fs:0,%r8\nmovq %r8,%rax\nret\n"
    ".size gap_indirect_thread_pointer,.-gap_indirect_thread_pointer\n"
    // OpenSSL has lookup tables in executable sections. These are data,
    // even though the bytes also encode a perfectly valid FS instruction.
    ".p2align 4\n.global gap_fs_like_table\n.type gap_fs_like_table,@object\n"
    "gap_fs_like_table:\n.byte 0x64,0x48,0x8b,0x04,0x25,0x28,0,0,0,0xc3\n"
    ".size gap_fs_like_table,.-gap_fs_like_table\n");

__attribute__((noinline)) static uintptr_t covered_guard(void) {
    uintptr_t guard;
    __asm__ volatile("movq %%fs:0x28,%0" : "=r"(guard));
    return guard;
}

static int failures;
static void check(const char* name, int ok) {
    printf("partial_unwind: %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

static void* worker(void* arg) {
    uintptr_t guard = covered_guard();
    check("new thread gets its own guard", guard != (uintptr_t)arg);
    check("uncovered RAX on new thread", gap_guard_rax() == guard);
    check("uncovered extended register on new thread", gap_guard_r8() == guard);
    check("indirect uncovered guard on new thread", indirect_guard() == guard);
    return NULL;
}

int gesher_selftest(void) {
    uintptr_t guard = covered_guard();
    check("covered guard is nonzero", guard != 0);
    check("uncovered RAX", gap_guard_rax() == guard);
    check("uncovered RCX", gap_guard_rcx() == guard);
    check("uncovered extended register", gap_guard_r8() == guard);
    check("MOV preserves flags", gap_guard_preserves_carry() == 1);
    check("hidden uncovered function reached by direct call", gap_guard_hidden() == guard);
    check("hidden uncovered function reached by indirect pointer", indirect_guard() == guard);
    check("indirect MOV preserves flags and extended registers", indirect_carry() == 1);
    uintptr_t thread_pointer;
    __asm__ volatile("movq %%fs:0,%0" : "=r"(thread_pointer));
    check("indirect thread pointer uses the actual TLS block", indirect_thread_pointer() == thread_pointer);
    check("runtime recovery leaves original code bytes intact",
        *(const volatile unsigned char*)(uintptr_t)indirect_guard == 0x64);
    static const unsigned char expected[] = {0x64,0x48,0x8b,0x04,0x25,0x28,0,0,0,0xc3};
    int intact = 1;
    for (unsigned i = 0; i < sizeof expected; ++i)
        if (gap_fs_like_table[i] != expected[i]) intact = 0;
    check("FS-like executable data is unchanged", intact);
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, worker, (void*)guard);
    check("thread creation", rc == 0);
    if (rc == 0) check("thread join", pthread_join(thread, NULL) == 0);
    if (failures == 0) puts("partial_unwind: all passed");
    return failures;
}
