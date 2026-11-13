// A valid DT_TEXTREL fixture for both native and translated ELF loading.
// Build with -nostdlib -Wl,-z,notext. The pointer table deliberately lives in
// a non-writable section, so loading it requires private writable pages during
// relocation and the PT_LOAD's final read-only protection before constructors.
//
// One slot names an exported symbol (an absolute dynamic relocation); the
// other names a hidden definition (a load-bias relocation).
const unsigned long gesher_textrel_value = 42;
__attribute__((visibility("hidden")))
const unsigned long gesher_textrel_local_value = 7;

__asm__(
    ".pushsection .rodata.gesher_textrel,\"a\"\n"
    ".balign 8\n"
    ".globl gesher_textrel_slots\n"
    ".hidden gesher_textrel_slots\n"
    "gesher_textrel_slots:\n"
    ".quad gesher_textrel_value\n"
    ".quad gesher_textrel_local_value\n"
    ".popsection\n");

extern const unsigned long* const volatile gesher_textrel_slots[2]
    __attribute__((visibility("hidden")));

static int check_slots(void) {
  const unsigned long* exported = gesher_textrel_slots[0];
  const unsigned long* local = gesher_textrel_slots[1];
  return exported != &gesher_textrel_value || local != &gesher_textrel_local_value ||
         *exported != 42 || *local != 7;
}

static int initialized;

__attribute__((constructor))
static void initialize(void) {
  initialized = check_slots() == 0 ? 1 : -1;
}

__attribute__((visibility("default")))
int gesher_selftest(void) {
  return initialized != 1 || check_slots() != 0;
}
