"""Build the explicitly limited Linux boundary for actual ARM libdl execution."""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def prepare(folder: Path, fixtures: Path, dependency: Path, build: Path) -> tuple[list[Path], list[str], list[str]]:
    translator = (ROOT / "compat/arm64/dll/translator.cc").read_text()
    libc = (ROOT / "compat/arm64/dll/guest_libc.cc").read_text()
    assembly = (ROOT / "compat/arm64/dll/thunks.S").read_text()

    def section(source: str, begin: str, end: str) -> str:
        if source.count(begin) != 1 or source.count(end) != 1:
            raise ValueError("ambiguous production section: " + begin + " / " + end)
        first = source.index(begin)
        return source[first:source.index(end, first)]

    sections = {
        "HOST_CALL": section(translator, "struct HostCall {", "// What gesher_guest_entry_shim"),
        "ENTRY_FRAME": section(translator, "struct EntryFrame {", "// thunks.S (Microsoft x64 entry):"),
        "FENV": section(translator, "class HostFenvScope {", "// --- guest code regions"),
        "CODE_REGIONS": section(translator, "struct CodeRegion {", "// --- guest TLS"),
        "MONITOR": section(translator, "constexpr size_t kMaxProcessors =", "size_t LiveProcessorIds()"),
        "VA_LIST": section(translator, "// Host list shape", "// Pages already found to hold host code"),
        "COUNTERS": section(translator, "struct Counter {", "class Env;\nstd::mutex g_envs_mutex;"),
        "JIT_AT": section(translator, "  A64::Jit* JitAt(size_t depth) {", "  A64::Jit* CurrentJit()"),
        "CALL_SVC": section(translator, "  void CallSVC(std::uint32_t swi) override {", "  void ExceptionRaised("),
        "ENTER": section(translator, "  uint64_t Enter(const void* fn,", "  // A guest program's _start"),
        "HOST_DISPATCH": section(translator, "  struct PendingHostCall {", "  std::vector<std::unique_ptr<A64::Jit>> jits_;"),
        "ENTER_GUEST": section(translator, "uint64_t EnterGuest(const void* fn,", "// --- host_entry stubs"),
        "RESULT_GETTERS": section(translator, "  uint64_t last_fp_result() const", "  bool OnGuestStack("),
        "HOST_ENTRY": section(translator, "std::mutex g_entry_mutex;", "// --- acquire loads on read-only memory"),
        "ENTRY_TRAMPOLINE": section(translator, 'extern "C" uint64_t gesher_guest_entry_trampoline(', "// A guest trampoline (guest code) that calls host function"),
        "API_CALL": section(translator, "uint64_t ApiCall(const void* fn,", "void* ApiHostEntry("),
        "CLASSIFIER": section(translator, "bool InGuestCode(uintptr_t addr) {", "uint32_t GuestFpcr()"),
        "CALLER_GETTER": section(translator, "const void* GuestCallerAddress() {", "void AddVaListFunction("),
        "BIND_HOST": section(translator, "void* BindHost(void* fn, int va_list_index) {", "bool InTranslatorCode("),
        "ADD_CODE": section(translator, "void AddGuestCode(const void* start, size_t size) {", "void* EmitGuestCode("),
        "EMIT_CODE": section(translator, "void* EmitGuestCode(const uint32_t* insns, size_t count) {", "// The calling thread's guest state"),
        "HOST_LOOKUP": section(libc, "void* Host(const char* name) {", "int* HostErrno()"),
        "GUEST_LIBDL": section(libc, "void* g_dlopen = nullptr;", "// --- setjmp / longjmp"),
        "OVERRIDE": section(libc, "void Override(const char* name, void* host_wrapper) {", "// A mismatched host"),
        "GUEST_SYMBOL": section(libc, "void* GuestSymbol(const char* name) {", "int64_t GuestRawSyscall("),
        "LIBDL_REGISTRATION": section(libc, '  g_dlopen = Host("__loader_dlopen");', "  for (const Fallback& f : kFenv)"),
        "RETURN_STUB": section(translator, "    uint32_t ret_stub[] =", "    uint32_t tlsdesc[] ="),
    }
    template = (fixtures / "guest_libdl_namespace_arm64.cc.in").read_text()
    generated = template
    print("ARM_BOUNDARY actual Dynarmic, production Enter/JitAt/CallSVC/RunGuest/CallHost/X30 getter/libdl/MakeHostEntry/full thunks; Linux attachment/storage and <=6-integer-argument host entries; no Windows DLL, TLS or syscall claim", flush=True)
    for name, source in (("complete_translator", translator), ("complete_guest_libc", libc),
                         ("complete_thunks", assembly), ("arm_template", template), *sections.items()):
        print("ARM_SHA256 " + name + " " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    for name, source in sections.items():
        marker = "@" + name + "@"
        if generated.count(marker) != 1:
            raise ValueError("ARM boundary marker changed: " + marker)
        generated = generated.replace(marker, source)
    unit = folder / "guest_libdl_arm64.cc"
    unit.write_text(generated)
    thunk = folder / "guest_libdl_thunk.S"
    thunk.write_text(assembly + '\n.section .note.GNU-stack,"",@progbits\n')
    print("ARM_SHA256 generated_boundary " + hashlib.sha256(generated.encode()).hexdigest(), flush=True)
    headers = fixtures / "arm64-dll"
    headers.mkdir()
    for name in ("translator.h", "guest_counter.h"):
        shutil.copy2(ROOT / "compat/arm64/dll" / name, headers / name)
        print("ARM_SHA256 " + name + " " + hashlib.sha256((headers / name).read_bytes()).hexdigest(), flush=True)
    libraries = [build / relative for relative in (
        "src/dynarmic/libdynarmic.a", "externals/fmt/libfmt.a", "externals/mcl/src/libmcl.a",
        "externals/zydis/libZydis.a", "externals/zydis/zycore/libZycore.a")]
    for library in libraries:
        print("DYNARMIC_LIBRARY_SHA256 " + str(library) + " " + hashlib.sha256(library.read_bytes()).hexdigest(), flush=True)
    for relative in ("tests/A64/testenv.h", "src/dynarmic/interface/A64/config.h"):
        print("DYNARMIC_HEADER_SHA256 " + relative + " " + hashlib.sha256((dependency / relative).read_bytes()).hexdigest(), flush=True)
    includes = ["-I", str(headers), "-I", str(dependency / "src"), "-I", str(dependency),
                "-I", str(dependency / "externals/mcl/include"), "-I", str(dependency / "externals/fmt/include")]
    link = ["-Wl,--start-group", *map(str, libraries), "-Wl,--end-group"]
    return [unit, thunk], includes, link
