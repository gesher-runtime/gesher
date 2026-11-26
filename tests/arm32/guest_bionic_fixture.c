// ARM32 guest ELF compiled by Zig Clang: real libc imports resolved by Bionic32.
// Intentional -nostdlib / -fno-builtin; this is not a full Android APK.
typedef unsigned int size_t;
extern void* malloc(size_t);
extern void* calloc(size_t, size_t);
extern void* realloc(void*, size_t);
extern void free(void*);
extern void* memset(void*, int, size_t);
extern void* memcpy(void*, const void*, size_t);
extern void* memmove(void*, const void*, size_t);
extern int memcmp(const void*, const void*, size_t);
extern size_t strlen(const char*);
extern size_t strnlen(const char*, size_t);
extern int strcmp(const char*, const char*);
extern int strncmp(const char*, const char*, size_t);
extern int* __errno(void);

// Keep string inputs in actual writable guest allocations.
int bionic_heap(void) {
  char* ptr = (char*)malloc(32);
  if (!ptr) return -1;
  memset(ptr, 'x', 8);
  ptr[8] = 0;
  if (strlen(ptr) != 8 || strnlen(ptr, 5) != 5) { free(ptr); return -2; }
  char* expanded = (char*)realloc(ptr, 8192);
  if (!expanded) { free(ptr); return -3; }
  if (expanded[0] != 'x' || expanded[7] != 'x' || expanded[8] != 0) {
    free(expanded); return -4;
  }
  free(expanded);
  return 42;
}

int bionic_copy(void) {
  char* left = (char*)calloc(1, 256);
  char* right = (char*)malloc(256);
  if (!left || !right) { free(left); free(right); return -1; }
  memset(left, 'a', 128);
  memcpy(right, left, 128);
  if (memcmp(left, right, 128) != 0) { free(left); free(right); return -2; }
  memmove(right + 8, right, 128);
  if (right[8] != 'a' || right[135] != 'a') {
    free(left); free(right); return -3;
  }
  free(left); free(right);
  return 77;
}

int bionic_strings(void) {
  char* left = (char*)malloc(32);
  char* right = (char*)malloc(32);
  if (!left || !right) { free(left); free(right); return -1; }
  memcpy(left, "hello", 6);
  memcpy(right, "help", 5);
  if (!(strcmp(left, right) < 0) ||
      !(strncmp(left, right, 3) == 0) ||
      !(strncmp(left, right, 5) < 0) ||
      strlen(left) != 5 || strnlen(left, 3) != 3) {
    free(left); free(right); return -2;
  }
  free(left); free(right);
  return 21;
}

int bionic_invalid(void) {
  // Must fail explicitly in the host adapter; no native host pointer access.
  return (int)strlen((const char*)0xff00);
}

int bionic_errno(void) {
  int* guest_errno = __errno();
  if (!guest_errno) return -1;
  *guest_errno = 0;
  if (malloc(0xffffffffu) != 0) return -2;
  if (*guest_errno != 12) return -3;  // Linux ENOMEM in guest memory.
  *guest_errno = 0;
  if (calloc(0x80000000u, 2) != 0) return -4;
  if (*guest_errno != 12) return -5;
  return 55;
}
