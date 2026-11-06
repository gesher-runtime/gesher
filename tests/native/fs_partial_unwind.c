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
    ".size gap_guard_preserves_carry,.-gap_guard_preserves_carry\n");

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
    return NULL;
}

int gesher_selftest(void) {
    uintptr_t guard = covered_guard();
    check("covered guard is nonzero", guard != 0);
    check("uncovered RAX", gap_guard_rax() == guard);
    check("uncovered RCX", gap_guard_rcx() == guard);
    check("uncovered extended register", gap_guard_r8() == guard);
    check("MOV preserves flags", gap_guard_preserves_carry() == 1);
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, worker, (void*)guard);
    check("thread creation", rc == 0);
    if (rc == 0) check("thread join", pthread_join(thread, NULL) == 0);
    if (failures == 0) puts("partial_unwind: all passed");
    return failures;
}
