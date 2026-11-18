# Compact ELF packed-relocation snapshots

## Scope and baseline

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.
The affected production path is `Relocate` in `compat/elf/elf_loader.cc`, used
when loading x86-64 and ARM64 ELF objects with `DT_ANDROID_RELA`/APS2 tables.

The old loader decoded the complete table into `std::vector<Elf64_Rela>` before
applying it. Each decoded entry takes 24 bytes. Highly compressed tables could
also repeatedly grow that vector because the old reserve heuristic rejected
counts greater than eight times the packed byte length.

The new loader owns a snapshot of the compact input bytes. It validates every
group and SLEB128 field before applying ordinary or packed RELA entries, then
reconstructs each relocation once as it is applied. Validation scans variable
field terminators without calculating every offset/addend; fully grouped
members have no per-member encoded fields and need no validation loop.
The application pass reads only that same validated, immutable snapshot.

This is an allocation and decode-work change in library loading. It does not
change Android APIs, supplied application inputs, symbol policy, capabilities,
permissions or execution choices.

## Preserved ordering and ownership

The original sequence is retained:

1. Apply RELR entries, including their effects on packed metadata.
2. Snapshot and validate packed relocations.
3. Apply ordinary RELA entries.
4. Apply packed entries in their encoded order.
5. Apply PLT entries.

A regular relocation, a packed relocation or an IFUNC/resolver may modify the
object's mapped APS2 bytes. The snapshot prevents those modifications from
changing the later decoded operations. Each load has independent local storage,
including reentrant and concurrent loads. No global cache or retained capacity
is introduced. An application failure stops the sequence at the same entry;
malformed packed input is rejected before ordinary RELA application, as before.

The four group flags, cumulative offset/addend rules, grouped-addend behavior
and callback order match the
[AOSP packed relocation iterator](https://android.googlesource.com/platform/bionic/+/master/linker/linker_reloc_iterators.h).
The change is authored from Gesher's existing decoder, rather than copying that
external implementation. Existing acceptance of trailing bytes and supported
group-flag combinations is retained. Addend accumulation uses unsigned 64-bit
arithmetic before converting its resulting bits back to the signed ELF field,
avoiding signed-overflow undefined behavior on corrupted inputs.

## Readable snapshot bounds

Before inspecting the APS2 signature or copying its declared section, the loader
checks the reservation bounds and actual readable segment coverage. A reserved
address gap in the file-view path does not count as readable memory. Adjacent
readable segments are accepted regardless of program-header order; unchecked
address or size arithmetic cannot authorize the snapshot. Bad signatures retain
the original header-only rejection behavior.

The follow-up host validation passed 60,444 ASan/UBSan checks, including 64 new
range-admission checks using ordinary owned arrays and declared mapping bounds.
It also retains the exact-baseline comparison and every existing allocation
budget. These checks exercise the production range validator at the declared
mapping boundary; they do not establish Windows virtual-memory execution.

## Measured allocation work

`tools/test_elf_packed_relocations.py` compiles the **unchanged production parser
and `Relocate` body** from the selected revision. The declared host boundary
observes architecture-specific relocation application and writes into a
preallocated destination; Windows mapping, real symbol/IFUNC execution and
loader-wide work are outside this measurement.

All workloads below are **synthetic**. The 281,843-entry size was inspired by an
existing source comment about a library's relocation count; no Waze APK or
`libwaze.so` was run or measured. The per-addend case uses grouped offsets and
type with individual addend deltas. Ungrouped stores all three fields individually;
mixed uses 64-entry groups alternating four grouping modes. Fully grouped is a
compression/growth stress case, not an assumed typical Android library.

One load of **281,843 relocations**:

| Encoding | Input bytes | Allocations before → after | Cumulative requested bytes before → after | Peak requested bytes before → after |
| --- | ---: | ---: | ---: | ---: |
| Per-addend | 281,857 | 1 → 1 | 6,764,232 → 281,857 | 6,764,232 → 281,857 |
| Ungrouped | 845,541 | 1 → 1 | 6,764,232 → 845,541 | 6,764,232 → 845,541 |
| Mixed groups | 581,322 | 1 → 1 | 6,764,232 → 581,322 | 6,764,232 → 581,322 |
| Fully grouped | 15 | 20 → 1 | 25,165,800 → 15 | 18,874,368 → 15 |

Per-addend temporary payload falls **95.8%**; ungrouped falls **87.5%** and mixed
falls **91.4%**. The fully grouped baseline's peak includes old and replacement
vector allocations simultaneously during growth. Cumulative allocation work
is distinct from live peak memory.

Counters observe requested C++ allocation payloads inside one production
`Relocate` call. Original source bytes, destination mappings, allocator
metadata, process RSS/commit, Windows mapping costs and real resolver costs
are excluded. Every workload verifies that the measured temporary storage is
freed on return. A compact byte snapshot remains necessary for ownership;
this is not zero-allocation loading. Unusually inefficient encodings or large
unused trailing sections can make the packed byte count exceed 24 bytes per
entry, so the compact-size bound is not a universal percentage guarantee.

## Informational host CPU timings

Linux x86-64, GCC 14.2.0, `-std=c++20 -O2 -fno-exceptions`, without sanitizers.
The table gives process CPU microseconds per call measured using `std::clock`,
the median of seven rounds. Each round repeats 32-entry loads 50,000 times,
4,096-entry loads 500 times and 281,843-entry loads 16 times. Concurrent test
workers have finished before measurement. Timings are not CI thresholds.

| Entries | Encoding | Baseline µs | Changed µs |
| ---: | --- | ---: | ---: |
| 32 | Per-addend | 0.226 | 0.215 |
| 32 | Fully grouped | 0.162 | 0.184 |
| 32 | Ungrouped | 0.299 | 0.293 |
| 32 | Mixed groups | 0.310 | 0.276 |
| 4,096 | Per-addend | 27.314 | 21.116 |
| 4,096 | Fully grouped | 41.750 | 19.846 |
| 4,096 | Ungrouped | 36.928 | 32.216 |
| 4,096 | Mixed groups | 33.140 | 27.492 |
| 281,843 | Per-addend | 2,412.938 | 1,511.125 |
| 281,843 | Fully grouped | 7,255.312 | 1,306.750 |
| 281,843 | Ungrouped | 3,142.438 | 2,161.562 |
| 281,843 | Mixed groups | 2,839.688 | 1,971.938 |

At the large size, the host fixture uses about **31–37% less CPU time** in the
three variable-field workloads and about 82% less in the fully grouped stress
case. The tiny fully grouped case differs by only 0.022 µs and does not show a
speed improvement. Host load and compiler optimization affect timings; other
runs varied. These figures do not establish application startup latency,
real Windows CPU/RSS, or a whole-app speedup.

The first prototype decoded both passes fully and regressed the medium
per-addend case. That implementation was replaced with the field-boundary
validation pass described above before publication.

## Verification and limits

- **60,380 checks pass with ASan/UBSan**, including the exact-baseline behavior
  comparison. Coverage includes all 16 flag combinations and transitions,
  signed fields, high-bit offsets/information, every truncation of representative
  tables of every flag type, invalid group counts and header-only rejection, RELR effects, unchanged
  failure prefixes, mapped-metadata overwrite, reentry and simultaneous loads.
- An independent encoder computes intended field values before emitting bytes.
  Both production revisions must produce those exact values in the same order.
- The changed parser also receives **20,000 seeded bounded corruptions** under
  sanitizers (40,000 assertions). These may create signed-addend overflow, which
  has no defined old-C++ oracle, so this corruption-only portion is excluded
  from old-source runs; the shared valid/malformed assertions and allocation
  budgets remain intact for the baseline control.
- Running the same allocation budgets on the exact baseline fails **14
  assertions**: all 12 compact-size bounds and the two growth-allocation bounds.
  Behavior/measurement execution of the baseline succeeds.
- Existing ELF lookup performance: **100,315 checks pass**; lookup contracts:
  **2,090 checks pass**; finalizers: **11 checks pass**, with ASan/UBSan.
- The complete production `elf_loader.cc` compiles for **Windows x64 and ARM64**
  with CI-pinned LLVM-MinGW `20261006`, `-Wall -Wextra -Werror -fno-exceptions`.
- The new sanitizer gate runs in unconditional native-contract CI preflight.
  `git diff --check` passes.

Local runs disable LeakSanitizer process inspection only, because this executor
uses ptrace; AddressSanitizer and UndefinedBehaviorSanitizer remain fatal and
the committed CI command retains normal sanitizer defaults. Fresh Windows/ART,
real APK loading and whole-runtime integration remain pending. Cross-compilation
and declared host adapters do not establish those results.

## Reproduce

```sh
python3 tools/test_elf_packed_relocations.py \
  --compare-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
python3 tools/test_elf_packed_relocations.py --sanitize \
  --compare-ref f93a90fac55dbe1aff641e6e3f26879292907cb2

# Expected nonzero exit, exactly 14 resource-budget failures:
python3 tools/test_elf_packed_relocations.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2

# Baseline behavior/measurement control, without the new resource budgets:
python3 tools/test_elf_packed_relocations.py --measure-only \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
```

At this revision, the complete production file SHA-256 is
`dcdec6646a35ba02e1259b629ea8352656c8a6ea1858855ea24371e31bb33f4d`;
the extracted changed decoder/dispatcher digest printed by the runner is
`b85c4d0655cc698c09bef32bf525e877caaa870feedb7aa72a2fc4783bb93e65`.
The exact baseline section digest is
`88e596e7cc4e13ef3aa251d7f34c07dc94d2b6e96a569e4ddc13a5ac03681e4f`.
