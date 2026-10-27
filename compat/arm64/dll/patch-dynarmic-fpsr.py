#!/usr/bin/env python3
"""Hash-check pinned x64 compiler and scalar ARM FZ input-status corrections.

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

"""
DN_INPUTS = """    if (ctx.FPCR().DN() || ctx.HasOptimization(OptimizationFlag::Unsafe_InaccurateNaN)) {
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(args[0]);
        const Xbyak::Xmm operand = ctx.reg_alloc.UseScratchXmm(args[1]);
"""
DN_RECORDED = DN_INPUTS + """
        if (ctx.FPCR().FZ()) {
            const Xbyak::Reg64 input_scratch = ctx.reg_alloc.ScratchGpr();
            RecordInputDenormal<fsize>(code, input_scratch, {result, operand});
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
    }

    SharedLabel end""")
EMITTER_REPLACEMENTS = [(THREE_OP, INPUT_HELPER + THREE_OP),
                        (DN_INPUTS, DN_RECORDED), (NORMAL_INPUTS, NORMAL_RECORDED)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def transform(data, relative=RELATIVE):
    if relative == RELATIVE:
        marker, original_sha, replacements = MARKER, ORIGINAL_SHA256, REPLACEMENTS
    elif relative == EMITTER:
        marker, original_sha, replacements = EMITTER_MARKER, EMITTER_SHA256, EMITTER_REPLACEMENTS
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
    for relative, original_sha in [(RELATIVE, ORIGINAL_SHA256), (EMITTER, EMITTER_SHA256)]:
        path = Path(args.source_root) / relative
        data = path.read_bytes()
        patched = transform(data, relative)
        path.write_bytes(patched)
        print("PINNED_DYNARMIC", PIN, relative, original_sha)
        print("PASS dynarmic FPSR source patch", relative, digest(patched))


if __name__ == "__main__":
    main()
