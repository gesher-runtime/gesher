// gesher ELF loader: thread-local storage (tls.h).

#include "tls.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gesher::elf {

namespace {

// bionic's x86-64 TCB slots (bionic/libc/platform/bionic/tls_defines.h).
constexpr size_t kSlotSelf = 0;        // %fs:0   = the thread pointer itself
constexpr size_t kSlotStackGuard = 5;  // %fs:0x28 = the stack-protector cookie
constexpr size_t kPage = 0x1000;
// Address space reserved below each thread pointer for static TLS; pages are
// committed as modules are assigned. Modules beyond it are dynamic-only.
constexpr size_t kStaticReserve = 1 << 20;
// The TCB page above the thread pointer.
constexpr size_t kTcbReserve = 0x10000;

// TEB64.TlsSlots: 64 inline slots; higher indices are not reachable with one
// %gs-relative load (the same constraint as ART's thread register, ADR 0006).
constexpr uint32_t kTebTlsSlots = 0x1480;
constexpr DWORD kInlineTlsSlots = 64;

size_t AlignUp(size_t v, size_t a) { return a <= 1 ? v : (v + a - 1) & ~(a - 1); }

// One thread's block.
struct Arena {
  uint8_t* reservation = nullptr;
  uintptr_t tp = 0;
  size_t committed = 0;              // bytes committed below tp
  // Provisional entries are guarded by g_mutex because rollback visits every
  // arena. A published entry moves to the thread-owned cache on its next use.
  std::unordered_map<size_t, uint8_t*> module_base;
  // Only the owning thread reads or writes this cache. Published modules cannot
  // be rolled back and their IDs are never reused, so no registry lock is
  // needed on an already cached access. Future successful-unload support must
  // establish its own invalidation contract before retiring these addresses.
  std::unordered_map<size_t, uint8_t*> published_base;
  std::vector<void*> dynamic;         // dynamic copies to free
};

struct ModuleRecord {
  TlsModule* module;
  DWORD owner_thread;  // only this thread may use or discard an unpublished module
  bool prepared = false;  // relocated image is usable by the owner before graph publication
};

std::mutex g_mutex;                 // guards everything below
size_t g_static_size = 0;           // highest live extent below TP
size_t g_next_module_id = 1;
std::unordered_map<size_t, ModuleRecord> g_modules;  // only live assigned modules
std::vector<Arena*> g_arenas;       // every live thread's block

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
DWORD g_slot_index = TLS_OUT_OF_INDEXES;
uint32_t g_slot_offset = 0;
DWORD g_fls = FLS_OUT_OF_INDEXES;  // per-thread Arena*, freed at thread exit

void FreeArena(Arena* a) {
  for (void* p : a->dynamic) _aligned_free(p);
  VirtualFree(a->reservation, 0, MEM_RELEASE);
  delete a;
}

void WINAPI ArenaDestructor(void* p) {
  if (p == nullptr) return;
  auto* a = static_cast<Arena*>(p);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_arenas.size(); ++i) {
      if (g_arenas[i] == a) {
        g_arenas.erase(g_arenas.begin() + static_cast<ptrdiff_t>(i));
        break;
      }
    }
  }
  if (g_slot_index != TLS_OUT_OF_INDEXES) TlsSetValue(g_slot_index, nullptr);
  FreeArena(a);
}

BOOL CALLBACK InitSlots(PINIT_ONCE, PVOID, PVOID*) {
  DWORD index = TlsAlloc();
  if (index != TLS_OUT_OF_INDEXES && index < kInlineTlsSlots) {
    g_slot_index = index;
    g_slot_offset = kTebTlsSlots + static_cast<uint32_t>(sizeof(void*)) * index;
  } else if (index != TLS_OUT_OF_INDEXES) {
    TlsFree(index);
  }
  g_fls = FlsAlloc(ArenaDestructor);
  return TRUE;
}

bool EnsureSlots() {
  InitOnceExecuteOnce(&g_once, InitSlots, nullptr, nullptr);
  return g_slot_offset != 0 && g_fls != FLS_OUT_OF_INDEXES;
}

// Commits `bytes` below a->tp. Under g_mutex.
bool CommitBelow(Arena* a, size_t bytes) {
  bytes = AlignUp(bytes, kPage);
  if (bytes <= a->committed) return true;
  void* p = VirtualAlloc(reinterpret_cast<void*>(a->tp - bytes), bytes - a->committed,
                         MEM_COMMIT, PAGE_READWRITE);
  if (p == nullptr) return false;
  a->committed = bytes;
  return true;
}

// Decommit only the unused tail below every surviving static module. Keep the
// old accounting if Windows refuses the decommit, so later reuse remains safe.
// Under g_mutex; also undoes commits made before a later arena's commit failed.
void TrimCommittedTail() {
  const size_t bytes = AlignUp(g_static_size, kPage);
  for (Arena* a : g_arenas) {
    if (a->committed > bytes &&
        VirtualFree(reinterpret_cast<void*>(a->tp - a->committed),
                    a->committed - bytes, MEM_DECOMMIT)) {
      a->committed = bytes;
    }
  }
}

void CopyImage(uint8_t* dst, const TlsModule* m) {
  if (m->init_size) std::memcpy(dst, m->init_image, m->init_size);
  if (m->total_size > m->init_size) {
    std::memset(dst + m->init_size, 0, m->total_size - m->init_size);
  }
}

Arena* BuildArena() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* a = new Arena();
  a->reservation = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, kStaticReserve + kTcbReserve, MEM_RESERVE, PAGE_NOACCESS));
  if (a->reservation == nullptr) {
    delete a;
    return nullptr;
  }
  a->tp = reinterpret_cast<uintptr_t>(a->reservation) + kStaticReserve;
  if (VirtualAlloc(reinterpret_cast<void*>(a->tp), kPage, MEM_COMMIT, PAGE_READWRITE) == nullptr ||
      !CommitBelow(a, g_static_size)) {
    FreeArena(a);
    return nullptr;
  }
  // TCB slots at and above TP. The stack guard is the same value the
  // rewritten %fs:0x28 reads (%gs:0x30, NT_TIB.Self), so code that reaches the
  // slot through the thread pointer agrees with the rewritten prologues.
  void** tcb = reinterpret_cast<void**>(a->tp);
  tcb[kSlotSelf] = reinterpret_cast<void*>(a->tp);
  tcb[kSlotStackGuard] = NtCurrentTeb();
  for (const auto& entry : g_modules) {
    const TlsModule* m = entry.second.module;
    if (m->is_static && (m->ready || (entry.second.prepared &&
                                    entry.second.owner_thread == GetCurrentThreadId()))) {
      auto* base = reinterpret_cast<uint8_t*>(a->tp + m->tp_offset);
      CopyImage(base, m);
      if (!m->ready) a->module_base.emplace(m->module_id, base);
    }
  }
  g_arenas.push_back(a);
  return a;
}

Arena* CurrentArena() {
  if (g_fls == FLS_OUT_OF_INDEXES) return nullptr;
  return static_cast<Arena*>(FlsGetValue(g_fls));
}

// The stack's bottom that stays as Windows keeps it (its guarantee, ART's
// protected region, fault_signals.cc's GrowStack rules), above the lowest page.
constexpr size_t kStackBottomKeep = 0x10000;

// Linux maps a thread's whole stack; Windows commits it a page at a time behind
// a guard page. Code built for Linux moves %rsp past many pages at once (a large
// frame, no probes: libonnxruntime.so's `sub $0x119c0,%rsp`), and with %rsp in
// reserved memory Windows cannot even deliver the fault: the process is ended,
// no handler runs. Commits the reserved pages above the bottom, once per thread
// that runs loaded code, with a guard page below them as Windows would have.
void CommitStack() {
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  const uintptr_t limit = reinterpret_cast<uintptr_t>(tib->StackLimit);
  const uintptr_t bottom = low + kStackBottomKeep;
  if (bottom + kPage >= limit || limit > high) return;
  for (uintptr_t p = bottom; p < limit;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == 0) return;
    uintptr_t end = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                                            mbi.RegionSize, limit);
    if (mbi.State == MEM_RESERVE) {
      if (VirtualAlloc(reinterpret_cast<void*>(p), end - p, MEM_COMMIT, PAGE_READWRITE) ==
          nullptr) {
        return;
      }
    } else if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) != 0) {
      DWORD old;
      VirtualProtect(reinterpret_cast<void*>(p), end - p, PAGE_READWRITE, &old);
    }
    p = end;
  }
  // A stack committed whole already (a thread created with a stack size to
  // commit) keeps its pages as they are.
  MEMORY_BASIC_INFORMATION below;
  if (VirtualQuery(reinterpret_cast<void*>(bottom - kPage), &below, sizeof(below)) != 0 &&
      below.State == MEM_RESERVE &&
      VirtualAlloc(reinterpret_cast<void*>(bottom - kPage), kPage, MEM_COMMIT,
                   PAGE_READWRITE | PAGE_GUARD) != nullptr) {
    tib->StackLimit = reinterpret_cast<void*>(bottom);
  }
}

}  // namespace

uint32_t TlsThreadPointerSlot() {
  EnsureSlots();
  return g_slot_offset;
}

uintptr_t TlsCurrentThreadPointer() {
  if (g_slot_index == TLS_OUT_OF_INDEXES) return 0;
  return reinterpret_cast<uintptr_t>(TlsGetValue(g_slot_index));
}

bool TlsAssignModule(TlsModule* m, bool executable) {
  if (m == nullptr) return false;
  if (!m->present) return true;
  if (m->init_size > m->total_size || (m->init_size != 0 && m->init_image == nullptr)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t align = m->align ? m->align : 1;
  if ((align & (align - 1)) != 0) return false;
  if (m->module_id != 0) {
    auto existing = g_modules.find(m->module_id);
    if (existing == g_modules.end() || existing->second.module != m ||
        (!m->ready && existing->second.owner_thread != GetCurrentThreadId())) return false;
    return !executable || (m->is_static && m->tp_offset == -static_cast<int64_t>(AlignUp(m->total_size, align)));
  }
  // Never wrap to a previously issued id, even after many failed loads.
  if (m->ready || g_next_module_id == 0) return false;
  if (executable && (g_static_size != 0 || align > kPage ||
                     m->total_size > kStaticReserve ||
                     (reinterpret_cast<uintptr_t>(m->init_image) & (align - 1)) != 0))
    return false;
  // Static offsets cannot move after publication. Find an aligned gap among
  // live extents instead of consuming a bump cursor: an outer failed load can
  // leave a hole above an independent module committed by its IFUNC callback.
  // Deriving gaps from the live registry also coalesces adjacent retired spans
  // without retaining allocation records that refer to discarded modules.
  bool fits = false;
  size_t extent = 0;
  if (align <= kPage && m->total_size <= kStaticReserve) {
    std::vector<std::pair<size_t, size_t>> occupied;
    occupied.reserve(g_modules.size());
    for (const auto& entry : g_modules) {
      const TlsModule* other = entry.second.module;
      if (!other->is_static) continue;
      const size_t end = static_cast<size_t>(-other->tp_offset);
      occupied.emplace_back(end - other->total_size, end);
    }
    std::sort(occupied.begin(), occupied.end());
    size_t cursor = 0;
    auto gap = [&](size_t end) {
      if (cursor > end || m->total_size > end - cursor) return false;
      extent = AlignUp(cursor + m->total_size, align);
      return extent <= end;
    };
    for (const auto& range : occupied) {
      if (gap(range.first)) { fits = true; break; }
      cursor = std::max(cursor, range.second);
    }
    if (!fits) fits = gap(kStaticReserve);
  }
  if (fits) {
    for (Arena* a : g_arenas) {
      if (!CommitBelow(a, std::max(g_static_size, extent))) {
        fits = false;
        TrimCommittedTail();
        break;
      }
    }
  }
  if (executable && !fits) return false;
  m->module_id = g_next_module_id++;
  if (fits) {
    m->is_static = true;
    m->tp_offset = -static_cast<int64_t>(extent);
    g_static_size = std::max(g_static_size, extent);
  } else {
    m->is_static = false;
    m->tp_offset = 0;
  }
  g_modules.emplace(m->module_id, ModuleRecord{m, GetCurrentThreadId()});
  return true;
}

bool TlsPrepareModule(TlsModule* m) {
  if (m == nullptr) return false;
  if (!m->present) return true;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto entry = g_modules.find(m->module_id);
  if (entry == g_modules.end() || entry->second.module != m || m->ready ||
      entry->second.owner_thread != GetCurrentThreadId()) return false;
  entry->second.prepared = true;
  // A parent's IFUNC can use a linked child's initial-exec TLS directly.
  // Prepare only this thread; a later Arena on this thread also sees the
  // prepared image. Foreign Arenas remain excluded until graph publication.
  Arena* a = CurrentArena();
  if (m->is_static && a != nullptr && a->module_base.count(m->module_id) == 0) {
    auto* base = reinterpret_cast<uint8_t*>(a->tp + m->tp_offset);
    CopyImage(base, m);
    a->module_base.emplace(m->module_id, base);
  }
  return true;
}

void TlsPublishModule(TlsModule* m) {
  if (m == nullptr || !m->present) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto entry = g_modules.find(m->module_id);
  if (entry == g_modules.end() || entry->second.module != m || m->ready ||
      entry->second.owner_thread != GetCurrentThreadId()) return;
  m->ready = true;
  if (!m->is_static) return;
  // Preserve private resolver writes on the owner. Other existing threads
  // receive the initial image only after the complete graph has succeeded.
  for (Arena* a : g_arenas) {
    if (a->module_base.count(m->module_id) == 0)
      CopyImage(reinterpret_cast<uint8_t*>(a->tp + m->tp_offset), m);
  }
}

bool TlsRollbackModule(TlsModule* m) {
  if (m == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto entry = g_modules.find(m->module_id);
  if (entry == g_modules.end() || entry->second.module != m || m->ready ||
      entry->second.owner_thread != GetCurrentThreadId()) return false;
  for (Arena* a : g_arenas) {
    auto cached = a->module_base.find(m->module_id);
    if (cached != a->module_base.end()) {
      if (!m->is_static) {
        auto dynamic = std::find(a->dynamic.begin(), a->dynamic.end(), cached->second);
        if (dynamic != a->dynamic.end()) a->dynamic.erase(dynamic);
        _aligned_free(cached->second);
      }
      a->module_base.erase(cached);
    }
    if (m->is_static && m->total_size != 0) {
      // A page can also contain a surviving module; clear only this allocation.
      std::memset(reinterpret_cast<void*>(a->tp + m->tp_offset), 0, m->total_size);
    }
  }
  g_modules.erase(entry);
  m->module_id = 0;
  m->tp_offset = 0;
  m->is_static = false;
  // An IFUNC may have committed a newer, independent library during this load.
  // Reclaim only space below all survivors, never a saved global checkpoint.
  g_static_size = 0;
  for (const auto& live : g_modules) {
    const TlsModule* other = live.second.module;
    if (other->is_static) {
      g_static_size = std::max(g_static_size, static_cast<size_t>(-other->tp_offset));
    }
  }
  TrimCommittedTail();
  return true;
}

int64_t TlsTpOffset(const TlsModule& m) { return m.tp_offset; }

bool TlsSetupThread() {
  if (!EnsureSlots()) return false;
  Arena* a = CurrentArena();
  if (a == nullptr) {
    a = BuildArena();
    if (a == nullptr) return false;
    FlsSetValue(g_fls, a);
    CommitStack();
  }
  TlsSetValue(g_slot_index, reinterpret_cast<void*>(a->tp));
  return true;
}

void* TlsGetAddr(size_t module_id, size_t offset) {
  Arena* a = CurrentArena();
  if (a == nullptr) {
    if (!TlsSetupThread()) return nullptr;
    a = CurrentArena();
  }
  const auto published = a->published_base.find(module_id);
  if (published != a->published_base.end()) return published->second + offset;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto entry = g_modules.find(module_id);
  if (entry == g_modules.end()) return nullptr;
  const TlsModule* m = entry->second.module;
  // Provisional cached addresses still require the owner check: an IFUNC can
  // resolve pending TLS, but another thread cannot observe that transaction.
  if (!m->ready && entry->second.owner_thread != GetCurrentThreadId()) return nullptr;
  auto cached = a->module_base.find(module_id);
  if (cached != a->module_base.end()) {
    uint8_t* base = cached->second;
    if (m->ready) a->published_base.insert(a->module_base.extract(cached));
    return base + offset;
  }
  uint8_t* base;
  if (m->is_static) {
    base = reinterpret_cast<uint8_t*>(a->tp + m->tp_offset);
    // General-dynamic access during the owner's relocation uses the initial
    // image, like a dynamic-only module. Caching preserves resolver writes
    // across later preparation and publication.
    if (!m->ready) CopyImage(base, m);
  } else {
    // Dynamic-only module: this thread's own copy, made on first use.
    size_t align = m->align < sizeof(void*) ? sizeof(void*) : m->align;
    base = static_cast<uint8_t*>(_aligned_malloc(m->total_size ? m->total_size : 1, align));
    if (base == nullptr) return nullptr;
    CopyImage(base, m);
    a->dynamic.push_back(base);
  }
  if (m->ready) a->published_base.emplace(module_id, base);
  else a->module_base.emplace(module_id, base);
  return base + offset;
}

}  // namespace gesher::elf
