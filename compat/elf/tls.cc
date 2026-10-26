// gesher ELF loader: thread-local storage (tls.h).

#include "tls.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
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
  std::vector<uint8_t*> module_base;  // by module id: tp + tp_offset, or a dynamic copy
  std::vector<void*> dynamic;         // dynamic copies to free
};

std::mutex g_mutex;                 // guards everything below
size_t g_static_size = 0;           // bytes assigned below TP so far
size_t g_next_module_id = 1;
std::vector<TlsModule*> g_modules;  // every assigned module
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
  for (TlsModule* m : g_modules) {
    if (m->is_static && m->ready) {
      CopyImage(reinterpret_cast<uint8_t*>(a->tp + m->tp_offset), m);
    }
  }
  g_arenas.push_back(a);
  return a;
}

Arena* CurrentArena() {
  if (g_fls == FLS_OUT_OF_INDEXES) return nullptr;
  return static_cast<Arena*>(FlsGetValue(g_fls));
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
  if (!m->present) return true;
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t align = m->align ? m->align : 1;
  if ((align & (align - 1)) != 0) return false;
  if (m->module_id != 0)
    return !executable || (m->is_static && m->tp_offset == -static_cast<int64_t>(AlignUp(m->total_size, align)));
  if (executable && (g_static_size != 0 || align > kPage ||
                     m->total_size > kStaticReserve ||
                     (reinterpret_cast<uintptr_t>(m->init_image) & (align - 1)) != 0))
    return false;
  size_t new_size = AlignUp(g_static_size + m->total_size, align);
  // The thread pointer is page-aligned, so any alignment up to a page holds.
  bool fits = new_size >= m->total_size && new_size <= kStaticReserve && align <= kPage;
  if (fits) {
    for (Arena* a : g_arenas) {
      if (!CommitBelow(a, new_size)) {
        fits = false;
        break;
      }
    }
  }
  if (executable && !fits) return false;
  m->module_id = g_next_module_id++;
  if (fits) {
    m->is_static = true;
    m->tp_offset = -static_cast<int64_t>(new_size);
    g_static_size = new_size;
  } else {
    m->is_static = false;
    m->tp_offset = 0;
  }
  g_modules.push_back(m);
  return true;
}

void TlsPublishModule(TlsModule* m) {
  if (!m->present) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (m->ready) return;
  m->ready = true;
  if (!m->is_static) return;
  // Threads that already have a block get the module's initial image now; its
  // code cannot have run on them yet (the object's init has not run).
  for (Arena* a : g_arenas) CopyImage(reinterpret_cast<uint8_t*>(a->tp + m->tp_offset), m);
}

int64_t TlsTpOffset(const TlsModule& m) { return m.tp_offset; }

bool TlsSetupThread() {
  if (!EnsureSlots()) return false;
  Arena* a = CurrentArena();
  if (a == nullptr) {
    a = BuildArena();
    if (a == nullptr) return false;
    FlsSetValue(g_fls, a);
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
  // Fast path: this thread already resolved the module.
  if (module_id < a->module_base.size() && a->module_base[module_id] != nullptr) {
    return a->module_base[module_id] + offset;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  TlsModule* m = nullptr;
  for (TlsModule* candidate : g_modules) {
    if (candidate->module_id == module_id) {
      m = candidate;
      break;
    }
  }
  if (m == nullptr) return nullptr;
  uint8_t* base;
  if (m->is_static) {
    base = reinterpret_cast<uint8_t*>(a->tp + m->tp_offset);
  } else {
    // Dynamic-only module: this thread's own copy, made on first use.
    size_t align = m->align < sizeof(void*) ? sizeof(void*) : m->align;
    base = static_cast<uint8_t*>(_aligned_malloc(m->total_size ? m->total_size : 1, align));
    if (base == nullptr) return nullptr;
    CopyImage(base, m);
    a->dynamic.push_back(base);
  }
  if (a->module_base.size() <= module_id) a->module_base.resize(module_id + 1, nullptr);
  a->module_base[module_id] = base;
  return base + offset;
}

}  // namespace gesher::elf
