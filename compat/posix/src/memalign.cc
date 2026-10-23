// gesher-base: posix_memalign() whose memory free() and realloc() accept.
//
// The CRT's free() takes only what malloc() returned, and _aligned_malloc()
// memory must go to _aligned_free(). posix_memalign() allocates with malloc(),
// returns an aligned pointer inside that block, and records the pair in a
// table. Every module links with --wrap=free and --wrap=realloc (posix.cmake),
// so free() and realloc() look a pointer up in the table while it is not
// empty; otherwise they go straight to the CRT.
//
// The table is per module: memory from posix_memalign() in one DLL must be
// freed by the same DLL (as with any static CRT).
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <atomic>

extern "C" void __real_free(void* ptr);
extern "C" void* __real_realloc(void* ptr, size_t size);

namespace {

// What malloc() already guarantees on x64.
constexpr size_t kMallocAlignment = 16;

struct Entry {
  uintptr_t aligned;  // 0: empty slot
  void* raw;
  size_t size;
};

SRWLOCK g_lock = SRWLOCK_INIT;
// Open addressing, linear probing, backward-shift deletion. The storage comes
// from HeapAlloc, never malloc/free, so the wrappers do not re-enter.
Entry* g_table = nullptr;
size_t g_capacity = 0;  // a power of two
size_t g_count = 0;
std::atomic<size_t> g_live{0};

size_t Slot(uintptr_t p, size_t mask) {
  uint64_t h = static_cast<uint64_t>(p >> 4) * 0x9E3779B97F4A7C15ull;
  return static_cast<size_t>(h ^ (h >> 32)) & mask;
}

void InsertNoGrow(const Entry& e) {
  size_t mask = g_capacity - 1;
  size_t i = Slot(e.aligned, mask);
  while (g_table[i].aligned != 0) i = (i + 1) & mask;
  g_table[i] = e;
}

bool Grow() {
  size_t capacity = g_capacity == 0 ? 64 : g_capacity * 2;
  auto* table = static_cast<Entry*>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(Entry)));
  if (table == nullptr) return false;
  Entry* old = g_table;
  size_t old_capacity = g_capacity;
  g_table = table;
  g_capacity = capacity;
  for (size_t i = 0; i < old_capacity; i++) {
    if (old[i].aligned != 0) InsertNoGrow(old[i]);
  }
  if (old != nullptr) HeapFree(GetProcessHeap(), 0, old);
  return true;
}

// Index of p's entry, or g_capacity. Called with the lock held.
size_t Find(uintptr_t p) {
  if (g_capacity == 0) return g_capacity;
  size_t mask = g_capacity - 1;
  for (size_t i = Slot(p, mask); g_table[i].aligned != 0; i = (i + 1) & mask) {
    if (g_table[i].aligned == p) return i;
  }
  return g_capacity;
}

// Called with the lock held (exclusive).
void Erase(size_t i) {
  size_t mask = g_capacity - 1;
  size_t j = i;
  for (;;) {
    j = (j + 1) & mask;
    if (g_table[j].aligned == 0) break;
    size_t k = Slot(g_table[j].aligned, mask);
    // The entry at j stays if its home slot k lies cyclically in (i, j].
    bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
    if (stays) continue;
    g_table[i] = g_table[j];
    i = j;
  }
  g_table[i].aligned = 0;
  g_count--;
  g_live.store(g_count, std::memory_order_release);
}

// If p came from posix_memalign(), removes it and returns its malloc() block.
void* Take(void* p) {
  AcquireSRWLockExclusive(&g_lock);
  void* raw = nullptr;
  size_t i = Find(reinterpret_cast<uintptr_t>(p));
  if (i != g_capacity) {
    raw = g_table[i].raw;
    Erase(i);
  }
  ReleaseSRWLockExclusive(&g_lock);
  return raw;
}

bool Lookup(void* p, size_t* size) {
  AcquireSRWLockShared(&g_lock);
  size_t i = Find(reinterpret_cast<uintptr_t>(p));
  bool found = i != g_capacity;
  if (found) *size = g_table[i].size;
  ReleaseSRWLockShared(&g_lock);
  return found;
}

bool Tracked(void* p) { return p != nullptr && g_live.load(std::memory_order_acquire) != 0; }

}  // namespace

extern "C" int posix_memalign(void** memptr, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return EINVAL;
  if (alignment <= kMallocAlignment) {
    void* p = malloc(size != 0 ? size : 1);
    if (p == nullptr) return ENOMEM;
    *memptr = p;
    return 0;
  }
  if (size > SIZE_MAX - alignment) return ENOMEM;
  void* raw = malloc(size + alignment - 1);
  if (raw == nullptr) return ENOMEM;
  uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + alignment - 1) & ~(alignment - 1);

  AcquireSRWLockExclusive(&g_lock);
  bool ok = (g_count + 1) * 2 <= g_capacity || Grow();
  if (ok) {
    InsertNoGrow(Entry{aligned, raw, size});
    g_count++;
    g_live.store(g_count, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (!ok) {
    __real_free(raw);
    return ENOMEM;
  }
  *memptr = reinterpret_cast<void*>(aligned);
  return 0;
}

extern "C" void __wrap_free(void* ptr) {
  if (Tracked(ptr)) {
    void* raw = Take(ptr);
    if (raw != nullptr) ptr = raw;
  }
  __real_free(ptr);
}

extern "C" void* __wrap_realloc(void* ptr, size_t size) {
  size_t old_size;
  if (Tracked(ptr) && Lookup(ptr, &old_size)) {
    // Like the CRT's realloc(p, 0): free p, return NULL.
    if (size == 0) {
      __wrap_free(ptr);
      return nullptr;
    }
    void* moved = malloc(size);
    if (moved == nullptr) return nullptr;  // ptr stays valid
    memcpy(moved, ptr, old_size < size ? old_size : size);
    __wrap_free(ptr);
    return moved;
  }
  return __real_realloc(ptr, size);
}
