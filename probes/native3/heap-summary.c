/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <windows.h>
#include <malloc.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef BOOL (WINAPI *SummaryFn)(HANDLE, DWORD, LPHEAP_SUMMARY);
static int failed;

static void check(int ok, const char* name) {
  printf("%s  native3 heap probe: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failed;
}

static int summarize(SummaryFn query, HANDLE heap, HEAP_SUMMARY* summary, const char* phase) {
  memset(summary, 0, sizeof(*summary));
  summary->cb = sizeof(*summary);
  LARGE_INTEGER before, after, frequency;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&before);
  BOOL ok = query(heap, 0, summary);
  DWORD error = ok ? 0 : GetLastError();
  QueryPerformanceCounter(&after);
  printf("INFO  native3 heap probe: %s ok=%d error=%lu allocated=%zu committed=%zu"
         " reserved=%zu query-ns=%" PRIu64 "\n", phase, ok, (unsigned long)error,
         summary->cbAllocated, summary->cbCommitted, summary->cbReserved,
         (uint64_t)((after.QuadPart - before.QuadPart) * INT64_C(1000000000) /
                    frequency.QuadPart));
  return ok;
}

static int walk(HANDLE heap, size_t* bytes, size_t* blocks) {
  if (!HeapLock(heap)) return 0;
  PROCESS_HEAP_ENTRY entry;
  memset(&entry, 0, sizeof(entry));
  *bytes = 0;
  *blocks = 0;
  while (HeapWalk(heap, &entry)) {
    if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
      *bytes += entry.cbData;
      ++*blocks;
    }
  }
  DWORD error = GetLastError();
  HeapUnlock(heap);
  return error == ERROR_NO_MORE_ITEMS;
}

int main(void) {
  SummaryFn query = (SummaryFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary");
  HANDLE heap = (HANDLE)_get_heap_handle();
  check(query != NULL, "HeapSummary is exported on this CI Windows version");
  check(heap != NULL && heap != INVALID_HANDLE_VALUE, "the CRT exposes its real heap handle");
  if (query == NULL || heap == NULL || heap == INVALID_HANDLE_VALUE) return 1;
  printf("INFO  native3 heap probe: CRT heap is process heap: %d\n", heap == GetProcessHeap());
  HEAP_SUMMARY before = {0}, allocated = {0}, resized = {0}, mapped = {0}, freed = {0};
  if (!summarize(query, heap, &before, "baseline")) return 1;
  const size_t size = 8u * 1024 * 1024;
  void* memory = malloc(size);
  check(memory != NULL, "the observed CRT allocation succeeds");
  if (memory == NULL) return 1;
  memset(memory, 0x5a, size);
  size_t usable = _msize(memory);
  check(usable != (size_t)-1 && usable >= size && summarize(query, heap, &allocated, "malloc") &&
            allocated.cbAllocated >= before.cbAllocated + size,
        "allocated heap bytes include a real malloc block");
  size_t walked_bytes = 0, walked_blocks = 0;
  check(walk(heap, &walked_bytes, &walked_blocks), "independent locked HeapWalk completes");
  printf("INFO  native3 heap probe: malloc usable=%zu summary=%zu walk=%zu blocks=%zu\n",
         usable, allocated.cbAllocated, walked_bytes, walked_blocks);
  void* grown = realloc(memory, 2 * size);
  check(grown != NULL, "the observed CRT realloc succeeds");
  if (grown == NULL) { free(memory); return 1; }
  memory = grown;
  check(summarize(query, heap, &resized, "realloc") &&
            resized.cbAllocated >= allocated.cbAllocated + size,
        "allocated heap bytes follow a real realloc growth");
  void* mapping = VirtualAlloc(NULL, 4 * size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  check(mapping != NULL, "the independent virtual mapping succeeds");
  if (mapping != NULL) {
    memset(mapping, 0x36, 4 * size);
    check(summarize(query, heap, &mapped, "VirtualAlloc") &&
              mapped.cbAllocated < resized.cbAllocated + size,
          "CRT heap accounting excludes a separate committed virtual mapping");
    VirtualFree(mapping, 0, MEM_RELEASE);
  }
  free(memory);
  check(summarize(query, heap, &freed, "free") &&
            resized.cbAllocated >= 2 * size &&
            freed.cbAllocated <= resized.cbAllocated - 2 * size,
        "allocated heap bytes decrease when the CRT block is freed");
  printf("%d test(s) failed\n", failed);
  return failed == 0 ? 0 : 1;
}
