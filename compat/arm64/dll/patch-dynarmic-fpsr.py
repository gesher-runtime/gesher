#!/usr/bin/env python3
"""Hash-check pinned x64 compiler and scalar ARM FZ status corrections,
memory corrections (A64 fast dispatch table, code cache commit, constant
pool) and the exclusive monitor's scan of the processor ids in use.

Only source rewriting/hashing runs locally. Compilation and all native
execution belong to CI. Preserve the original dynarmic 0BSD source/license.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys

PIN = "9d4582339990d4eae53f1dc7160686920fc2075c"
RELATIVE = "src/dynarmic/backend/x64/a64_interface.cpp"
ORIGINAL_SHA256 = "4757e74b518304a2e996ac98f80bed3483319ac6463b55bae29e05f984bc01ec"
MARKER = "Gesher FPSR: compilation is not guest arithmetic."
OLD_BLOCK = """        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        constexpr size_t MINIMUM_REMAINING_CODESIZE"""
NEW_BLOCK = """        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        // Gesher FPSR: compilation is not guest arithmetic. Dispatcher cache
        // misses can enter here with live guest MXCSR. Keep compiler math
        // independent of guest rounding and restore every saved status bit.
        const unsigned compiler_mxcsr = _mm_getcsr();
        SCOPE_EXIT { _mm_setcsr(compiler_mxcsr); };
        _mm_setcsr(0x1f80);

        constexpr size_t MINIMUM_REMAINING_CODESIZE"""
REPLACEMENTS = [("#include <mutex>\n", "#include <mutex>\n#include <xmmintrin.h>\n"),
                (OLD_BLOCK, NEW_BLOCK)]

EMITTER = "src/dynarmic/backend/x64/emit_x64_floating_point.cpp"
EMITTER_SHA256 = "f8b69a24b9ce98478e84ce444d61ebfcad40ae0947776f2553306f9e36d0bdb2"
EMITTER_MARKER = "Gesher FPSR: ARM FZ input-denormal status."
THREE_OP = "template<size_t fsize, typename Function>\nvoid FPThreeOp("
INPUT_HELPER = """// Gesher FPSR: ARM FZ input-denormal status. x86 DAZ flushes inputs
// without setting ARM IDC. Classify scalar operand bits before the existing
// arithmetic/NaN path; keep both the numeric values and NaN priority intact.
template<size_t fsize>
void RecordInputDenormal(BlockOfCode& code, const Xbyak::Reg64& scratch,
                         std::initializer_list<Xbyak::Xmm> operands) {
    static_assert(fsize == 32 || fsize == 64);
    for (const Xbyak::Xmm& operand : operands) {
        Xbyak::Label not_denormal;
        if constexpr (fsize == 32) {
            code.movd(scratch.cvt32(), operand);
            code.shl(scratch.cvt32(), 1);  // Remove sign; zero is not denormal.
            code.jz(not_denormal, code.T_NEAR);
            code.shr(scratch.cvt32(), 24);  // The eight exponent bits.
        } else {
            code.movq(scratch, operand);
            code.shl(scratch, 1);
            code.jz(not_denormal, code.T_NEAR);
            code.shr(scratch, 53);  // The eleven exponent bits.
        }
        code.jnz(not_denormal, code.T_NEAR);
        code.or_(code.dword[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc], 0x80);
        code.L(not_denormal);
    }
}

// ARM FZ output flushing raises UFC without IXC. Isolate this instruction's
// hardware underflow/inexact flags, then merge the complete prior status.
// This also preserves IXC from an earlier instruction in the same block.
void BeginFzStatus(BlockOfCode& code, EmitContext& ctx, const Xbyak::Reg64& scratch) {
    ctx.reg_alloc.AllocStackSpace(16);
    code.stmxcsr(code.dword[code.rsp]);
    code.mov(scratch.cvt32(), code.dword[code.rsp]);
    code.and_(scratch.cvt32(), ~0x30u);
    code.mov(code.dword[code.rsp + 4], scratch.cvt32());
    code.ldmxcsr(code.dword[code.rsp + 4]);
}

void EndFzStatus(BlockOfCode& code, EmitContext& ctx, const Xbyak::Reg64& scratch) {
    Xbyak::Label not_underflow;
    code.stmxcsr(code.dword[code.rsp + 4]);
    code.mov(scratch.cvt32(), code.dword[code.rsp + 4]);
    code.test(scratch.cvt32(), 0x10);  // x86 underflow from this operation.
    code.jz(not_underflow, code.T_NEAR);
    code.and_(scratch.cvt32(), ~0x20u);  // Its FTZ inexact is not ARM IXC.
    code.L(not_underflow);
    code.or_(scratch.cvt32(), code.dword[code.rsp]);
    code.mov(code.dword[code.rsp + 4], scratch.cvt32());
    code.ldmxcsr(code.dword[code.rsp + 4]);
    ctx.reg_alloc.ReleaseStackSpace(16);
}

"""
DN_INPUTS = """    if (ctx.FPCR().DN() || ctx.HasOptimization(OptimizationFlag::Unsafe_InaccurateNaN)) {
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(args[0]);
        const Xbyak::Xmm operand = ctx.reg_alloc.UseScratchXmm(args[1]);
"""
DN_RECORDED = DN_INPUTS + """
        const Xbyak::Reg64 status_scratch = ctx.FPCR().FZ() ? ctx.reg_alloc.ScratchGpr() : code.rax;
        if (ctx.FPCR().FZ()) {
            RecordInputDenormal<fsize>(code, status_scratch, {result, operand});
            BeginFzStatus(code, ctx, status_scratch);
        }
"""
NORMAL_INPUTS = """    const Xbyak::Xmm op1 = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm op2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr();

    SharedLabel end = GenSharedLabel(), nan = GenSharedLabel();"""
NORMAL_RECORDED = NORMAL_INPUTS.replace(
    "    SharedLabel end", """    if (ctx.FPCR().FZ()) {
        RecordInputDenormal<fsize>(code, tmp, {op1, op2});
        BeginFzStatus(code, ctx, tmp);
    }

    SharedLabel end""")
DN_ARITHMETIC_END = """            fn(result, operand);
        }

        if (!ctx.HasOptimization(OptimizationFlag::Unsafe_InaccurateNaN)) {
            ForceToDefaultNaN<fsize>(code, result);"""
DN_STATUS_END = DN_ARITHMETIC_END.replace(
    "        if (!ctx.HasOptimization", """        if (ctx.FPCR().FZ()) {
            EndFzStatus(code, ctx, status_scratch);
        }

        if (!ctx.HasOptimization""")
NORMAL_ARITHMETIC_END = """        fn(result, op2);
    }
    FCODE(ucomis)(result, result);"""
NORMAL_STATUS_END = NORMAL_ARITHMETIC_END.replace(
    "    FCODE(ucomis)", """    if (ctx.FPCR().FZ()) {
        EndFzStatus(code, ctx, tmp);
    }
    FCODE(ucomis)""")
EMITTER_REPLACEMENTS = [(THREE_OP, INPUT_HELPER + THREE_OP),
                        (DN_INPUTS, DN_RECORDED), (NORMAL_INPUTS, NORMAL_RECORDED),
                        (DN_ARITHMETIC_END, DN_STATUS_END),
                        (NORMAL_ARITHMETIC_END, NORMAL_STATUS_END)]

# Each A64 Jit (one per guest thread, plus one per nested guest call) holds a
# fast dispatch table that dynarmic clears on creation, so all of it is in the
# working set: 16 MiB per thread at the pinned size, hundreds of MB for an app
# with many native threads. A32's size (64Ki entries, 1 MiB) keeps the lookup.
DISPATCH = "src/dynarmic/backend/x64/a64_emit_x64.h"
DISPATCH_SHA256 = "3ab331c1bf170deec2370d3d1b1277e88e4ccd37e6affabd755e1887d2767109"
DISPATCH_MARKER = "Gesher memory: A64 fast dispatch table of 64Ki entries."
DISPATCH_OLD = """    static constexpr u64 fast_dispatch_table_mask = 0xFFFFF0;
    static constexpr size_t fast_dispatch_table_size = 0x100000;"""
DISPATCH_NEW = """    // Gesher memory: A64 fast dispatch table of 64Ki entries. The pinned 1Mi
    // entries (16 MiB, all cleared on creation) were resident per Jit.
    static constexpr u64 fast_dispatch_table_mask = 0xFFFF0;
    static constexpr size_t fast_dispatch_table_size = 0x10000;"""
# The table was cleared entry by entry to 0xFFFF... (an impossible descriptor)
# when each Jit was created, so all of it was resident even if the Jit ran a
# few blocks. Entries now hold the descriptor's complement: the empty entry is
# all zero, the table is its own VirtualAlloc (zero, committed lazily by the
# OS) and clearing it decommits it. Untouched pages are never in the set.
DISPATCH_ENTRY_OLD = """    struct FastDispatchEntry {
        u64 location_descriptor = 0xFFFF'FFFF'FFFF'FFFFull;
        const void* code_ptr = nullptr;
    };"""
DISPATCH_ENTRY_NEW = """    // Gesher memory: location_descriptor holds ~descriptor, so the empty
    // entry (descriptor ~0, which never occurs) is all zero.
    struct FastDispatchEntry {
        u64 location_descriptor = 0;
        const void* code_ptr = nullptr;
    };"""
DISPATCH_TABLE_OLD = """    std::array<FastDispatchEntry, fast_dispatch_table_size> fast_dispatch_table;"""
DISPATCH_TABLE_NEW = """    // Zero pages of its own (block_of_code.cpp), so a clear is a decommit and
    // only the entries a Jit uses become resident.
    struct FastDispatchTable {
        FastDispatchTable();
        ~FastDispatchTable();
        FastDispatchTable(const FastDispatchTable&) = delete;
        FastDispatchTable& operator=(const FastDispatchTable&) = delete;
        FastDispatchEntry* data() const { return entries; }
        void Clear();
        FastDispatchEntry* entries;
    };
    FastDispatchTable fast_dispatch_table;"""
DISPATCH_DECL_OLD = """namespace Dynarmic::Backend::X64 {

class RegAlloc;"""
DISPATCH_DECL_NEW = """namespace Dynarmic::Backend::X64 {

// Gesher memory: zero-filled pages committed on first touch (block_of_code.cpp).
void* AllocateZeroPages(size_t size);
void ZeroPages(void* pointer, size_t size);
void FreeZeroPages(void* pointer, size_t size);

class RegAlloc;"""
DISPATCH_REPLACEMENTS = [(DISPATCH_OLD, DISPATCH_NEW), (DISPATCH_ENTRY_OLD, DISPATCH_ENTRY_NEW),
                         (DISPATCH_TABLE_OLD, DISPATCH_TABLE_NEW),
                         (DISPATCH_DECL_OLD, DISPATCH_DECL_NEW)]

# The dispatcher compares and stores ~descriptor (rcx is free there: the
# descriptor calculation already clobbers it, and LookupBlock is a call).
EMIT = "src/dynarmic/backend/x64/a64_emit_x64.cpp"
EMIT_SHA256 = "450e9bc6c6497c053e9f39223542b0d3cc247bec358783ce178615f4d65827b4"
EMIT_MARKER = "Gesher memory: the table holds ~descriptor."
EMIT_LOOKUP_OLD = """        code.cmp(rbx, qword[rbp + offsetof(FastDispatchEntry, location_descriptor)]);
        code.jne(fast_dispatch_cache_miss);
        code.jmp(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)]);
        code.L(fast_dispatch_cache_miss);
        code.mov(qword[rbp + offsetof(FastDispatchEntry, location_descriptor)], rbx);"""
EMIT_LOOKUP_NEW = """        // Gesher memory: the table holds ~descriptor. The empty entry is zero.
        code.mov(rcx, rbx);
        code.not_(rcx);
        code.cmp(rcx, qword[rbp + offsetof(FastDispatchEntry, location_descriptor)]);
        code.jne(fast_dispatch_cache_miss);
        code.jmp(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)]);
        code.L(fast_dispatch_cache_miss);
        code.mov(qword[rbp + offsetof(FastDispatchEntry, location_descriptor)], rcx);"""
EMIT_CLEAR_OLD = """        fast_dispatch_table.fill({});
    }
}
"""
EMIT_CLEAR_NEW = """        fast_dispatch_table.Clear();
    }
}

A64EmitX64::FastDispatchTable::FastDispatchTable()
        : entries(static_cast<FastDispatchEntry*>(AllocateZeroPages(sizeof(FastDispatchEntry) * fast_dispatch_table_size))) {}

A64EmitX64::FastDispatchTable::~FastDispatchTable() {
    FreeZeroPages(entries, sizeof(FastDispatchEntry) * fast_dispatch_table_size);
}

void A64EmitX64::FastDispatchTable::Clear() {
    ZeroPages(entries, sizeof(FastDispatchEntry) * fast_dispatch_table_size);
}
"""
EMIT_REPLACEMENTS = [(EMIT_LOOKUP_OLD, EMIT_LOOKUP_NEW), (EMIT_CLEAR_OLD, EMIT_CLEAR_NEW)]

# Each Jit's code cache also committed 16 MiB up front; dynarmic commits more
# (1 MiB ahead) before compiling each block anyway, so commit 4 MiB to start.
CODE = "src/dynarmic/backend/x64/block_of_code.cpp"
CODE_SHA256 = "978bca9c0d112b602bc99f5a5762d17350615dd6d5a9c4220054b98f679ccfd5"
CODE_MARKER = "Gesher memory: commit the code cache as it fills."
CODE_OLD = "constexpr size_t PRELUDE_COMMIT_SIZE = 16 * 1024 * 1024;"
CODE_NEW = """// Gesher memory: commit the code cache as it fills.
constexpr size_t PRELUDE_COMMIT_SIZE = 4 * 1024 * 1024;"""
# The constant pool (2 MiB of code space, before the prelude) was cleared by
# AllocateFromCodeSpace although fresh committed pages are already zero: all
# of it became resident. Before the prelude, code space is only ever handed
# out above the insertion point, never written: clear only after it. A nested
# Jit's smaller cache gets a proportionally smaller pool (1/16th; the
# outermost Jit's 32 MiB cache keeps the pinned 2 MiB).
CODE_POOL_OLD = """        , constant_pool(*this, CONSTANT_POOL_SIZE)"""
CODE_POOL_NEW = """        , constant_pool(*this, std::min<size_t>(CONSTANT_POOL_SIZE, total_code_size / 16))"""
CODE_MEMSET_OLD = """    size_ += alloc_size;
    memset(ret, 0, alloc_size);
    return ret;"""
CODE_MEMSET_NEW = """    size_ += alloc_size;
    if (prelude_complete) {  // before it, fresh committed pages: already zero
        memset(ret, 0, alloc_size);
    }
    return ret;"""
CODE_PAGES_OLD = """void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {"""
CODE_PAGES_NEW = """void* AllocateZeroPages(size_t size) {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = nullptr;
#endif
    ASSERT_MSG(p != nullptr, "cannot allocate the fast dispatch table");
    return p;
}

void ZeroPages(void* pointer, size_t size) {
#ifdef _WIN32
    // Decommitted pages read back as zero once committed again, and stay out
    // of the working set until touched.
    if (!VirtualFree(pointer, size, MEM_DECOMMIT) ||
        VirtualAlloc(pointer, size, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        std::memset(pointer, 0, size);
    }
#else
    if (madvise(pointer, size, MADV_DONTNEED) != 0) std::memset(pointer, 0, size);
#endif
}

void FreeZeroPages(void* pointer, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(pointer, 0, MEM_RELEASE);
#else
    munmap(pointer, size);
#endif
}

void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {"""
CODE_REPLACEMENTS = [(CODE_OLD, CODE_NEW), (CODE_POOL_OLD, CODE_POOL_NEW),
                     (CODE_MEMSET_OLD, CODE_MEMSET_NEW), (CODE_PAGES_OLD, CODE_PAGES_NEW)]

# Exclusive monitor: the translator gives every live Jit its own processor id
# (two threads sharing one overwrote each other's reservation and saved LDXR
# value, so a store-exclusive could succeed with the other thread's value)
# from a monitor sized for thousands. A store-exclusive scanned every slot
# under the global lock; it now scans only the ids in use.
MONITOR_H = "src/dynarmic/interface/exclusive_monitor.h"
MONITOR_H_SHA256 = "18d8e7befc18631f5b3c5d0294d7cffe8185bc1e3695d9976f055f1c2fcd45e1"
MONITOR_H_MARKER = "Gesher: only processors [0, count) are in use"
MONITOR_H_OLD = """    void ClearProcessor(size_t processor_id);
"""
MONITOR_H_NEW = """    void ClearProcessor(size_t processor_id);
    /// Gesher: only processors [0, count) are in use (count <= GetProcessorCount());
    /// a store-exclusive clears the matching reservations among those. Slots
    /// leaving the range are unmarked. For callback (non-inline) exclusives.
    void SetActiveProcessorCount(size_t count);
"""
MONITOR_H_FIELD_OLD = """    std::vector<Vector> exclusive_values;
"""
MONITOR_H_FIELD_NEW = """    std::vector<Vector> exclusive_values;
    size_t active_processor_count = ~size_t{0};
"""
MONITOR_H_REPLACEMENTS = [(MONITOR_H_OLD, MONITOR_H_NEW), (MONITOR_H_FIELD_OLD, MONITOR_H_FIELD_NEW)]

MONITOR = "src/dynarmic/backend/x64/exclusive_monitor.cpp"
MONITOR_SHA256 = "d2cab03fe8f5cceb229a9566f0291f4270c339eb019f36218e798e7aa579921a"
MONITOR_MARKER = "Gesher: scan only the processors in use."
MONITOR_SCAN_OLD = """    for (VAddr& other_address : exclusive_addresses) {
        if (other_address == masked_address) {
            other_address = INVALID_EXCLUSIVE_ADDRESS;
        }
    }
    return true;
}
"""
MONITOR_SCAN_NEW = """    // Gesher: scan only the processors in use.
    const size_t active = std::min(active_processor_count, exclusive_addresses.size());
    for (size_t i = 0; i < active; ++i) {
        VAddr& other_address = exclusive_addresses[i];
        if (other_address == masked_address) {
            other_address = INVALID_EXCLUSIVE_ADDRESS;
        }
    }
    return true;
}

void ExclusiveMonitor::SetActiveProcessorCount(size_t count) {
    Lock();
    count = std::min(count, exclusive_addresses.size());
    const size_t active = std::min(active_processor_count, exclusive_addresses.size());
    for (size_t i = count; i < active; ++i) {
        exclusive_addresses[i] = INVALID_EXCLUSIVE_ADDRESS;
    }
    active_processor_count = count;
    Unlock();
}
"""
MONITOR_REPLACEMENTS = [(MONITOR_SCAN_OLD, MONITOR_SCAN_NEW)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def transform(data, relative=RELATIVE):
    if relative == RELATIVE:
        marker, original_sha, replacements = MARKER, ORIGINAL_SHA256, REPLACEMENTS
    elif relative == EMITTER:
        marker, original_sha, replacements = EMITTER_MARKER, EMITTER_SHA256, EMITTER_REPLACEMENTS
    elif relative == DISPATCH:
        marker, original_sha, replacements = DISPATCH_MARKER, DISPATCH_SHA256, DISPATCH_REPLACEMENTS
    elif relative == CODE:
        marker, original_sha, replacements = CODE_MARKER, CODE_SHA256, CODE_REPLACEMENTS
    elif relative == EMIT:
        marker, original_sha, replacements = EMIT_MARKER, EMIT_SHA256, EMIT_REPLACEMENTS
    elif relative == MONITOR_H:
        marker, original_sha, replacements = MONITOR_H_MARKER, MONITOR_H_SHA256, MONITOR_H_REPLACEMENTS
    elif relative == MONITOR:
        marker, original_sha, replacements = MONITOR_MARKER, MONITOR_SHA256, MONITOR_REPLACEMENTS
    else:
        raise ValueError("unknown pinned source")
    text = data.decode("utf-8")
    if marker in text:
        original = text
        for old, new in reversed(replacements):
            if original.count(new) != 1:
                raise ValueError("partial or changed compiler patch")
            original = original.replace(new, old, 1)
        if digest(original.encode()) != original_sha:
            raise ValueError("patched source does not reconstruct the pinned original")
        return data
    if digest(data) != original_sha:
        raise ValueError("compiler source is not the exact pinned original")
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError("pinned compiler patch anchor changed")
        text = text.replace(old, new, 1)
    return text.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", nargs="?")
    parser.add_argument("--verify-input-json", action="store_true")
    args = parser.parse_args()
    if args.verify_input_json:
        items = json.load(sys.stdin)
        if isinstance(items, dict):
            items = [items]
        for item in items:
            relative = item.get("relative", RELATIVE)
            data = base64.b64decode(item["base64"])
            patched = transform(data, relative)
            assert transform(patched, relative) == patched
            print("PASS source-only patch provenance, anchors and idempotence", relative, digest(patched))
        return
    if not args.source_root:
        parser.error("source_root is required")
    for relative, original_sha in [(RELATIVE, ORIGINAL_SHA256), (EMITTER, EMITTER_SHA256),
                                   (DISPATCH, DISPATCH_SHA256), (CODE, CODE_SHA256),
                                   (EMIT, EMIT_SHA256), (MONITOR_H, MONITOR_H_SHA256),
                                   (MONITOR, MONITOR_SHA256)]:
        path = Path(args.source_root) / relative
        data = path.read_bytes()
        patched = transform(data, relative)
        path.write_bytes(patched)
        print("PINNED_DYNARMIC", PIN, relative, original_sha)
        print("PASS dynarmic FPSR source patch", relative, digest(patched))


if __name__ == "__main__":
    main()
