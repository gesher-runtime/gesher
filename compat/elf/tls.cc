// gesher ELF loader: thread-local storage (tls.h).

#include "tls.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace gesher::elf {

namespace {

// bionic's x86-64 TCB slots (bionic/libc/platform/bionic/tls_defines.h).
constexpr size_t kSlotSelf = 0;        // %fs:0   = the thread pointer itself
constexpr size_t kSlotStackGuard = 5;  // %fs:0x28 = the stack-protector cookie
constexpr size_t kTcbSlots = 16;       // bionic reserves a handful; be generous
constexpr size_t kTcbBytes = kTcbSlots * sizeof(void*);

// A constant stack cookie. bionic randomises it; any fixed non-zero value is
// correct here, since the prologue and epilogue both read %fs:0x28.
constexpr uintptr_t kStackGuard = 0x0123456789abcdefULL;

// Static TLS arena below the thread pointer. All load-time modules share it.
std::mutex g_mutex;
size_t g_static_size = 0;  // bytes assigned below TP so far
size_t g_next_module_id = 1;
std::vector<TlsModule*> g_modules;  // every assigned module, for new threads

// Availability of WRFSBASE, decided once.
int g_wrfsbase = -1;  // -1 unknown, 0 no, 1 yes
bool g_fault = false;

LONG CALLBACK UdFilter(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
    g_fault = true;
    info->ContextRecord->Rip += 5;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void WriteFsBase(uintptr_t v) {
  __asm__ __volatile__("wrfsbase %0" : : "r"(v) : "memory");
}

bool WrfsbaseAvailable() {
  if (g_wrfsbase >= 0) return g_wrfsbase == 1;
  g_fault = false;
  PVOID h = AddVectoredExceptionHandler(1, UdFilter);
  uintptr_t dummy = 0;
  WriteFsBase(reinterpret_cast<uintptr_t>(&dummy));
  RemoveVectoredExceptionHandler(h);
  if (g_fault) {
    g_wrfsbase = 0;
  } else {
    g_wrfsbase = 1;
    WriteFsBase(0);  // leave it clean until a thread is actually set up
  }
  return g_wrfsbase == 1;
}

size_t AlignUp(size_t v, size_t a) { return a <= 1 ? v : (v + a - 1) & ~(a - 1); }

// Per-thread arena. Windows keeps %fs base per thread, but we also remember the
// arena pointer so repeated setup and __tls_get_addr work. This thread_local
// uses Windows TLS (%gs/TEB), independent of the %fs we set for Android code.
thread_local uint8_t* t_arena = nullptr;  // base of the whole arena
thread_local uintptr_t t_tp = 0;          // the thread pointer (%fs base)

uint8_t* BuildArena() {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Layout: [ static TLS (g_static_size, grows down from TP) ][ TP -> TCB ].
  size_t static_size = AlignUp(g_static_size, 64);
  size_t total = static_size + kTcbBytes;
  auto* arena = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, AlignUp(total, 0x1000), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (arena == nullptr) return nullptr;
  uintptr_t tp = reinterpret_cast<uintptr_t>(arena) + static_size;

  // TCB slots at and above TP.
  void** tcb = reinterpret_cast<void**>(tp);
  tcb[kSlotSelf] = reinterpret_cast<void*>(tp);
  tcb[kSlotStackGuard] = reinterpret_cast<void*>(kStackGuard);

  // Copy each module's init image into [TP + tp_offset, ...).
  for (TlsModule* m : g_modules) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(tp + m->tp_offset);
    if (m->init_size) std::memcpy(dst, m->init_image, m->init_size);
    if (m->total_size > m->init_size) {
      std::memset(dst + m->init_size, 0, m->total_size - m->init_size);
    }
  }
  t_arena = arena;
  t_tp = tp;
  return arena;
}

}  // namespace

void TlsAssignModule(TlsModule* m) {
  if (!m->present || m->module_id != 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  m->module_id = g_next_module_id++;
  // Place the module's block just below the current bottom of the static area.
  size_t align = m->align ? m->align : 1;
  size_t new_size = AlignUp(g_static_size + m->total_size, align);
  m->tp_offset = -static_cast<int64_t>(new_size);
  g_static_size = new_size;
  g_modules.push_back(m);
}

int64_t TlsTpOffset(const TlsModule& m) { return m.tp_offset; }

bool TlsSetupThread() {
  if (!WrfsbaseAvailable()) return false;
  if (t_arena == nullptr) {
    if (BuildArena() == nullptr) return false;
  }
  WriteFsBase(t_tp);
  return true;
}

void* TlsGetAddr(size_t module_id, size_t offset) {
  if (t_arena == nullptr) {
    if (!TlsSetupThread()) return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (TlsModule* m : g_modules) {
    if (m->module_id == module_id) {
      return reinterpret_cast<uint8_t*>(t_tp + m->tp_offset) + offset;
    }
  }
  return nullptr;
}

}  // namespace gesher::elf
