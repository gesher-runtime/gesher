# First-product selected supplier ledger proposal (source only)

This separate worktree starts at `0cf4106cd0636073b0fb28f360409b3a5a83d81d`.
The factory worktree remains frozen. No main/default supplier, workflow,
product implementation, trusted-source guard or Web4 implementation is changed
by this helper proposal. There is no CI/publication approval for it.

The independently qualified selected TCP consumer remains source `695229f0`,
run `41139659911/1`, current selected static archive `b415358c...`.
Its original publisher control still fails the first detached census by +1.
Neither result is relabeled as a full product adoption.

## API and boundaries

`selected_link.compiler_identity(configured_driver)` returns `None` immediately
for the unchanged publisher/default mode, without opening a receipt, resolving
a driver or running a command. Unknown modes fail. Explicit selected mode
requires the factory's existing exact `ci/build` caller/context and fresh-root
receipt, recipe and on-disk authority checks. Its result is intended as the
additive `compiler.selected_toolchain` field in `host_roles.compiler()`.
It includes current source/run/attempt, receipt/authority/recipe hashes,
aliases, public headers and original profile. It excludes executable and link
trace hashes. Thus production OFF and hook ON keep the existing compiler
equality contract.

`selected_link.cmake` is an explicit selected-only `CMAKE_PROJECT_INCLUDE`;
it is not included by the default toolchain. It sets supported C/CXX linker
launchers and defers target enumeration until the root graph is complete.
Executable/shared/module targets receive explicit `--verbose` and a map only
when their existing target options do not already specify one. Existing
dalvikvm/dex2oat/oatdump map flags and option order remain intact. The launcher
strictly rejects absent/duplicate maps or verbose flags rather than choosing
the last one. Actual CMake-generated argv and map forms remain CI obligations.

`selected_link.py launch <driver> <original arguments...>` executes exactly
that argv with no shell and no inserted/reordered compiler flags. It captures
the complete output of that one target's child, exit status, original and
audit-expanded argv, response bytes/hashes, output hash, actual map and an
actual `llvm-readobj --coff-imports` result. The explicit CMake options supply
diagnostics; the launcher does not infer a link from search directories.

The initial supported response form is the ordinary flat GNU response file
on the Linux producer. LLVM's GNU quote/escape grammar differs from POSIX
`shlex`; the helper uses the former. It preserves the exact execution argv,
checks captured bytes again before returning, and later replays those saved
bytes because Ninja may delete the response file. Nested files, BOM/UTF16,
invalid UTF8, NUL and unfinished quoting fail closed; none silently become a
missing link. Broader formats require separate actual-toolchain evidence.

The static proof correlates actual full-path `Reading` records, `Loaded
archive(member)` records and live provider-symbol map owners/addresses. The
pinned LLD GNU writer uses **bare member names** in its map. Every such owner
must resolve uniquely across **all** actually loaded archive members before
the selected archive is accepted. A real direct `.o`/`.obj` Reading path with
the same basename is an ambiguity too, and rejects the claim without changing
the captured command or response paths. Archive-prefixed map owners are unsupported
here; a synthetic `archive(member)` positive in the first unpublished helper
was rejected by independent peer and replaced by full actual-map replay.
Full-path contradictions, ambiguous aliases, another provider object,
dynamic pthread imports, different owners or addresses fail. Exact duplicate
symbol/owner/address rows coalesce, matching the already reviewed LLD behavior.
Provider names and each symbol's defining member come from the current
supplier's actual nm inventory (including the actual local TLS callback and
anchor). A matching archive hash and invented or wrong
member cannot establish a definition. Other live global symbols actually
owned by a selected member (for example clock-only use) are bound too, while
an ordinary product clock definition does not count as provider use. The role remains the exact selected
static product supplier; dynamic product adoption is not introduced here.

`NOT-LINKED` requires an actual successful captured target link, nonempty map
and x64 PE import capture, with neither selected Loaded members nor provider
definitions/imports. Missing capture, map, imports or a loaded member whose
live definition cannot be proved is a hard error.

`seal(original_output, published_output, compiler_record)` checks the current
authority and exact final output, replays the saved actual command and proof,
and copies the exact receipt and trace/map/import evidence beside the already
copied or moved executable. It returns a separate per-output `selected_link`
index containing record/output/receipt hashes and status. Seal production
before the hook relink; the latter gets its own record even when the original
build path is reused. `verify_sealed()` is a portable, process-free receiver
check of those archived bytes, command, current receipt identity and proof.
Linux producer paths use pure POSIX normalization during Windows verification.

## Next integration, after independent interface peer

No existing call site is wired in this first helper freeze. The proposed
bounded follow-up is:

1. Route the setup action's explicit selected receipt/context to the first
   `ci/build` job and select this CMake include only in that action's selected
   configure branch. Publisher commands and all other jobs stay exact.
2. Add only the stable optional compiler field to `host_roles.compiler()`;
   add/seal per-output records at production/hook and ordinary Binder capture.
   The existing OFF/ON compiler equality remains unchanged.
3. Seal the three actual Native outputs before computing their whole-file
   inventory; extend the selected-only inventory to the exact evidence files
   and receipt, and call the portable verifier. Default Native member sets
   and manifest bytes remain unchanged.
4. Route the first job's standalone teardown-observer link through the same
   launcher with explicit selected-only diagnostic flags. This is preservation
   tooling; its source/behavior and flagged Web4 implementation are untouched.
5. Add mechanical default-record/default-command equality and strict selected
   inventory/context/record ablations. Do not relax existing source pins:
   their reviewed future adoption remains a separate prerequisite to dispatch.

This helper does not close the stronger provider-clock, detach/native late
retirement, cancellation/nonlocal unwind, TSD ordering, loader/bootstrap,
async or independent-root adoption gates recorded in the canonical map.

## Source validation

`test_selected_link.py --historical-authority
C:/work/gesher-ci-review/tcp-selected-411396-current-authority.json` pins the
actual old authority hash/source/run, then uses visibly SYNTHETIC map/command
metadata only. It checks linked/unlinked positives, identical duplicates,
29 origin/member/import/owner/address/map proof negatives, two mode failures,
six command failures, eight response failures and 13 portable sealed-record
negatives. No compiler/native process
or network operation is started by this test. Actual new CMake/Ninja links
and final map/member/receipt/Native receiver evidence remain unexecuted.

The additional `--actual-producer-log
C:/work/gesher-ci-review/pthread-411350-124335330294.log --actual-authority
C:/work/gesher-ci-review/pthread-411350-minimum-supplier-reconstructed.json`
mode pins both complete saved inputs and reconstructs the entire actual
static control trace/map/import blocks. The corrected helper accepts their
16,013/67,513/4,273 bytes and binds 142 live definitions through seven selected
members. All 18 prior actual member/address/archive bindings compare equal.
Seven actual-derived missing-origin, ambiguous-origin, other-supplier,
globally-ambiguous-member, unsupported-map-owner and conflicting-address
and direct-object collision controls reject. This is historical diagnostic archive `5c53...` only; it
does not relabel those bytes as the fresh selected `b415...` archive, perform
a new link or qualify the new launcher/CMake integration. On a future actual
proof failure the launcher prints the full raw map/import rows before raising.

Primary implementation references consulted: CMake 3.24
[`<LANG>_LINKER_LAUNCHER`](https://cmake.org/cmake/help/v3.24/prop_tgt/LANG_LINKER_LAUNCHER.html),
[`CMAKE_PROJECT_INCLUDE`](https://cmake.org/cmake/help/v3.24/variable/CMAKE_PROJECT_INCLUDE.html),
[`cmake_language(DEFER)`](https://cmake.org/cmake/help/v3.24/command/cmake_language.html),
and LLVM 23.1.3
[`TokenizeGNUCommandLine`](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/Support/CommandLine.cpp#L777).
