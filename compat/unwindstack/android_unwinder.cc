// Gesher: libunwindstack's AndroidUnwinder over Windows' own x64 unwinder.
//
// libutilscallstack (CallStack, ProcessCallStack) unwinds through
// unwindstack::AndroidLocalUnwinder. Windows keeps the x64 unwind data of
// every function (.pdata/.xdata, which clang emits for mingw too), so the
// frames come from RtlVirtualUnwind: the calling thread's from
// RtlCaptureContext, another thread's from GetThreadContext while it is
// suspended. Function names come from each module's COFF symbol table (kept
// by unstripped executables and DLLs) or, failing that, its export table.
// Frames are formatted as Unwinder::FormatFrame does on Android:
//   "  #00 pc 0000000000012345  C:/path/module.exe (function+offset)"
// with pc relative to the module's base, as rel_pc is to an ELF's.
//
// Remote unwinding (another process) reports ERROR_UNSUPPORTED.

#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/Error.h>

// <windows.h> defines ERROR_INVALID_PARAMETER as Win32's error number.
constexpr unwindstack::ErrorCode kErrorInvalidParameter = unwindstack::ERROR_INVALID_PARAMETER;

#include <cxxabi.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Symbol {
  uint32_t rva;
  std::string name;
};

struct ModuleSymbols {
  std::string path;
  std::vector<Symbol> symbols;  // sorted by rva
};

std::string Narrow(const wchar_t* s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

// Reads the function symbols of a module's COFF symbol table from its file.
void ReadCoffSymbols(const std::wstring& path, ModuleSymbols* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size;
  std::vector<uint8_t> data;
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 31)) {
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) ||
        read != data.size()) {
      data.clear();
    }
  }
  CloseHandle(file);
  if (data.size() < sizeof(IMAGE_DOS_HEADER)) return;
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
      static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > data.size()) {
    return;
  }
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(data.data() + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  const IMAGE_FILE_HEADER& fh = nt->FileHeader;
  auto sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
      reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) + fh.SizeOfOptionalHeader);
  if (reinterpret_cast<const uint8_t*>(sections + fh.NumberOfSections) >
      data.data() + data.size()) {
    return;
  }
  size_t symtab = fh.PointerToSymbolTable;
  size_t count = fh.NumberOfSymbols;
  if (symtab == 0 || count == 0 || symtab + count * IMAGE_SIZEOF_SYMBOL > data.size()) return;
  const uint8_t* strings = data.data() + symtab + count * IMAGE_SIZEOF_SYMBOL;
  size_t strings_size = data.size() - (symtab + count * IMAGE_SIZEOF_SYMBOL);
  for (size_t i = 0; i < count; ++i) {
    IMAGE_SYMBOL sym;
    memcpy(&sym, data.data() + symtab + i * IMAGE_SIZEOF_SYMBOL, sizeof(sym));
    size_t aux = sym.NumberOfAuxSymbols;
    i += aux;
    if (sym.SectionNumber <= 0 || sym.SectionNumber > fh.NumberOfSections) continue;
    if (sym.StorageClass != IMAGE_SYM_CLASS_EXTERNAL &&
        sym.StorageClass != IMAGE_SYM_CLASS_STATIC) {
      continue;
    }
    const IMAGE_SECTION_HEADER& section = sections[sym.SectionNumber - 1];
    if ((section.Characteristics & IMAGE_SCN_CNT_CODE) == 0) continue;
    // Section symbols (".text") and labels are static with no type; keep
    // functions, and externals of any type.
    if (sym.StorageClass == IMAGE_SYM_CLASS_STATIC && ISFCN(sym.Type) == 0) continue;
    std::string name;
    if (sym.N.Name.Short != 0) {
      name.assign(reinterpret_cast<const char*>(sym.N.ShortName),
                  strnlen(reinterpret_cast<const char*>(sym.N.ShortName), 8));
    } else {
      size_t offset = sym.N.Name.Long;
      if (offset >= strings_size) continue;
      name.assign(reinterpret_cast<const char*>(strings + offset),
                  strnlen(reinterpret_cast<const char*>(strings + offset), strings_size - offset));
    }
    if (name.empty() || name[0] == '.') continue;
    out->symbols.push_back({section.VirtualAddress + sym.Value, std::move(name)});
  }
}

// The names a module exports, for modules without a symbol table.
void ReadExports(HMODULE module, ModuleSymbols* out) {
  auto base = reinterpret_cast<const uint8_t*>(module);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return;
  auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
  auto functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
    DWORD rva = functions[ordinals[i]];
    // Forwarders point into the export directory, not at code.
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;
    out->symbols.push_back({rva, reinterpret_cast<const char*>(base + names[i])});
  }
}

std::mutex modules_mutex;

const ModuleSymbols& SymbolsOf(HMODULE module) {
  [[clang::no_destroy]] static std::map<HMODULE, ModuleSymbols> modules;
  std::lock_guard<std::mutex> lock(modules_mutex);
  auto it = modules.find(module);
  if (it != modules.end()) return it->second;
  ModuleSymbols& m = modules[module];
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) {
      path.clear();
      break;
    }
    if (n < path.size()) {
      path.resize(n);
      break;
    }
    path.resize(path.size() * 2);
  }
  m.path = Narrow(path.c_str());
  std::replace(m.path.begin(), m.path.end(), '\\', '/');
  if (!path.empty()) ReadCoffSymbols(path, &m);
  if (m.symbols.empty()) ReadExports(module, &m);
  std::sort(m.symbols.begin(), m.symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
  return m;
}

HMODULE ModuleOf(uint64_t pc) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(pc), &module)) {
    return nullptr;
  }
  return module;
}

// Fills in a frame's module-relative pc and function from its pc.
void Symbolize(unwindstack::FrameData* frame) {
  HMODULE module = ModuleOf(frame->pc);
  if (module == nullptr) {
    frame->rel_pc = frame->pc;
    return;
  }
  uint64_t rva = frame->pc - reinterpret_cast<uint64_t>(module);
  frame->rel_pc = rva;
  const ModuleSymbols& m = SymbolsOf(module);
  auto it = std::upper_bound(m.symbols.begin(), m.symbols.end(), rva,
                             [](uint64_t v, const Symbol& s) { return v < s.rva; });
  if (it == m.symbols.begin()) return;
  --it;
  frame->function_name = it->name;
  frame->function_offset = rva - it->rva;
}

std::string Demangle(const std::string& name) {
  // mingw's C++ names are Itanium's, sometimes with the Windows decoration's
  // leading underscore.
  const char* mangled = name.c_str();
  if (strncmp(mangled, "__Z", 3) == 0) ++mangled;
  if (strncmp(mangled, "_Z", 2) != 0) return name;
  int status = 0;
  char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  if (demangled == nullptr) return name;
  std::string result(demangled);
  free(demangled);
  return result;
}

constexpr size_t kMaxPcs = 512;

// Walks the stack from ctx, recording each frame's pc and sp. Allocates
// nothing: it runs while another thread may be suspended holding the heap's
// lock.
size_t Walk(CONTEXT* ctx, uint64_t* pcs, uint64_t* sps, size_t max) {
  size_t n = 0;
  while (n < max && ctx->Rip != 0) {
    pcs[n] = ctx->Rip;
    sps[n] = ctx->Rsp;
    ++n;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &image_base, nullptr);
    if (fn == nullptr) {
      // A leaf function: the return address is at the top of the stack.
      if (ctx->Rsp == 0) break;
      ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
      ctx->Rsp += 8;
    } else {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      uint64_t old_sp = ctx->Rsp;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx->Rip, fn, ctx, &handler_data,
                       &establisher, nullptr);
      if (ctx->Rsp <= old_sp && ctx->Rip == pcs[n - 1]) break;  // no progress
    }
  }
  return n;
}

}  // namespace

namespace unwindstack {

void AndroidUnwinderData::DemangleFunctionNames() {
  for (auto& frame : frames) {
    frame.function_name = Demangle(frame.function_name);
  }
}

std::string AndroidUnwinderData::GetErrorString() {
  std::string error_msg(GetErrorCodeString(error.code));
  if (error.address != 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), " at address 0x%" PRIx64, error.address);
    error_msg += buf;
  }
  return error_msg;
}

AndroidUnwinder* AndroidUnwinder::Create(pid_t pid) {
  if (pid == getpid()) {
    return new AndroidLocalUnwinder;
  } else {
    return new AndroidRemoteUnwinder(pid);
  }
}

bool AndroidUnwinder::Initialize(ErrorData& error) {
  std::call_once(initialize_, [this, &error]() { initialize_status_ = InternalInitialize(error); });
  return initialize_status_;
}

std::string AndroidUnwinder::FormatFrame(const FrameData& frame) const {
  if (arch_ == ARCH_UNKNOWN) return "";
  char buf[64];
  snprintf(buf, sizeof(buf), "  #%02zu pc %016" PRIx64, frame.num, frame.rel_pc);
  std::string data(buf);
  HMODULE module = ModuleOf(frame.pc);
  if (module == nullptr) {
    data += "  <unknown>";
  } else {
    data += "  ";
    data += SymbolsOf(module).path;
  }
  if (!frame.function_name.empty()) {
    data += " (" + Demangle(frame.function_name);
    if (frame.function_offset != 0) {
      snprintf(buf, sizeof(buf), "+%" PRId64, frame.function_offset);
      data += buf;
    }
    data += ')';
  }
  return data;
}

FrameData AndroidUnwinder::BuildFrameFromPcOnly(uint64_t pc) {
  FrameData frame{};
  frame.pc = pc;
  Symbolize(&frame);
  return frame;
}

bool AndroidUnwinder::Unwind(AndroidUnwinderData& data) {
  return Unwind(std::nullopt, data);
}

bool AndroidUnwinder::Unwind(std::optional<pid_t> tid, AndroidUnwinderData& data) {
  if (!Initialize(data.error)) return false;
  return InternalUnwind(tid, data);
}

bool AndroidUnwinder::Unwind(void* ucontext, AndroidUnwinderData& data) {
  if (ucontext == nullptr) {
    data.error.code = kErrorInvalidParameter;
    return false;
  }
  // A signal handler's ucontext_t has no Windows unwinding context in it.
  data.error.code = ERROR_UNSUPPORTED;
  return false;
}

bool AndroidUnwinder::Unwind(Regs*, AndroidUnwinderData& data) {
  data.error.code = ERROR_UNSUPPORTED;
  return false;
}

bool AndroidLocalUnwinder::InternalInitialize(ErrorData&) {
  arch_ = ARCH_X86_64;
  return true;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t> tid, AndroidUnwinderData& data) {
  data.frames.clear();
  data.error.code = ERROR_NONE;
  uint64_t pcs[kMaxPcs];
  uint64_t sps[kMaxPcs];
  size_t max = std::min(data.max_frames.value_or(max_frames_), kMaxPcs);
  size_t n = 0;
  bool self = !tid.has_value() || static_cast<DWORD>(*tid) == GetCurrentThreadId();
  if (self) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    n = Walk(&ctx, pcs, sps, max + 8);
  } else {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, static_cast<DWORD>(*tid));
    if (thread == nullptr) {
      data.error.code = ERROR_THREAD_DOES_NOT_EXIST;
      return false;
    }
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
      CloseHandle(thread);
      data.error.code = ERROR_SYSTEM_CALL;
      return false;
    }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    bool ok = GetThreadContext(thread, &ctx) != 0;
    if (ok) n = Walk(&ctx, pcs, sps, max);
    ResumeThread(thread);
    CloseHandle(thread);
    if (!ok) {
      data.error.code = ERROR_SYSTEM_CALL;
      return false;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    FrameData frame{};
    frame.pc = pcs[i];
    frame.sp = sps[i];
    Symbolize(&frame);
    // The unwinder's own frames, as Android skips libunwindstack.so's.
    if (self && data.frames.empty() &&
        std::string_view(frame.function_name).find("unwindstack") != std::string_view::npos) {
      continue;
    }
    if (data.frames.size() == max) {
      data.error.code = ERROR_MAX_FRAMES_EXCEEDED;
      break;
    }
    frame.num = data.frames.size();
    data.frames.push_back(std::move(frame));
  }
  return !data.frames.empty();
}

bool AndroidRemoteUnwinder::InternalInitialize(ErrorData& error) {
  error.code = ERROR_UNSUPPORTED;
  return false;
}

bool AndroidRemoteUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData& data) {
  data.error.code = ERROR_UNSUPPORTED;
  return false;
}

}  // namespace unwindstack
