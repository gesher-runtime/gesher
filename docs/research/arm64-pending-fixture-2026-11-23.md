# ARM64 pending rollback fixture composition

The maintained pending-load rollback fixture now compiles against #432's translator API and passes its original contracts. The repair changes two fixture files. Runtime sources, workflows, source manifests, and approval predicates remain byte-identical to the author head. **The separate selected-source rejection remains unresolved; this repair does not approve #432's `fsync` fallback or socket changes.**

## Actual failure and exact inputs

The [artifact-preflight job](https://github.com/gesher-runtime/gesher/actions/runs/45276310619/job/138051494312) failed while compiling `tools/test_arm64_pending_rollback.py` at author head `4b30aac29c5e88dcc5841769544d52acbd136d46`. Its generated checkout was `763594f818042e494eb9d9d61b6a75f889d4adc0`. Both translator files and all three fixture inputs are byte-identical between that checkout and the author head; the source proof and complete original logs are archived.

The old adapter lacked the new live-Jit registry dependencies, `posted_` counters, the halt reason returned from `Jit::Run`, `Dynarmic::Has`/`CacheInvalidation`, and the two helpers called from the actual rollback/dispatch code. Running the unchanged maintained entry locally reproduces its compiler failure. The original generated translation unit and diagnostics are retained; it never reached the runtime contracts.

## Narrow repair

`tools/test_arm64_pending_rollback.py` extracts the complete current `ForgetGuestProtection` and `FlushIcache` bodies from `translator.cc`, using unique source anchors. All twelve earlier extraction boundaries remain exact. No production body is replaced or rewritten.

The host adapter adds the required `<map>` include and halt-result API. It owns each new posted counter beside its Jit, registers both through the actual registry helper, and unregisters the Jit before either owned object is destroyed. The synthetic cache still models the existing whole-cache retirement behavior. Its new `InvalidateCacheRange` method explicitly fails if called: **targeted guest instruction-cache coherence is not modeled or validated by this fixture.** The actual `kSigIll` declaration comes from the translator header.

Every original test body and its 96 inline `REQUIRE` sites after the shared-TLS marker remain byte-for-byte intact. The shared TLS corpus is also unchanged. A single new final assertion requires the actual live-Jit registry to be empty after all adapter owners retire. The existing mandatory CI invocation still runs this entry with `--sanitize`.

## Validation and boundaries

| Check | Observed result |
|---|---|
| Exact original maintained entry | Compilation fails at the new API boundary, matching the archived failure |
| Repaired ordinary entry | All 13 original contract groups pass |
| Repaired ASan/UBSan entry | The same 13 groups pass, with no sanitizer finding |
| Complete generated source | Ordinary and sanitizer TUs are identical: `4e8789d3999ac3e60974a0ecda60271470b6c37ae3491374b163120aa3696f17` |
| Registry lifetime control | Removing only adapter unregistering compiles, then fails the final empty-registry assertion |
| Untested-invalidation guard control | Calling the actual `FlushIcache` body with a nonempty range reaches the adapter's deliberate failure |
| Silent no-op negative | Replacing only that boundary by a no-op returns successfully and is rejected by the same fail-closed guard expectation |
| Source/composition proof | 138 source, extraction, original-assertion, log, and controlled-outcome checks pass |
| Existing selected-source gate | Still rejects `compat/posix/src/unistd.cc`; its policy and manifests are unchanged |

The helper's documentation was rewrapped between the ordinary and sanitizer runs; their complete generated C++ source is identical. The sanitizer run uses the final helper bytes. The retained receipts identify both helper revisions, compiler command lines, generated source and executable hashes.

These are host adapter results. The adapter uses real owned POSIX mappings as an explicit substitute for the Windows mapping boundary, does not execute generated guest instructions, and controls Jit dispatch with callbacks. No actual Windows/ARM64 translation, targeted cache-invalidation correctness, full artifact-preflight pass, or full-CI success follows from these results. LeakSanitizer is disabled for the shared host environment; address and undefined-behavior checks remain enabled.

## Separate protected-source failure

The [selected-source job](https://github.com/gesher-runtime/gesher/actions/runs/45276310619/job/138051494718) fails in `preceding_ofd_sources` before selected compilation. Our unchanged local entry produces the same `unistd.cc` identity rejection. The original 71-source manifest also feeds the reviewed OFD transition, whose `socket.cc`, `socket_internal.h`, and `winsock.cc` inputs changed on this author branch. Updating one digest would not establish the complete transition's scope or preserve the original qualification authority.

The author's [current `fsync` path](https://github.com/gesher-runtime/gesher/blob/4b30aac29c5e88dcc5841769544d52acbd136d46/compat/posix/src/unistd.cc) returns the result of `WriteBackCachedData` after a writable reopen and a writable-alias flush are unavailable. That helper can return zero after `FlushViewOfFile`; it does not call `FlushFileBuffers`. Microsoft's [documented native contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile) excludes file metadata flushing and completion through the hardware disk cache from `FlushViewOfFile` alone. The PR already calls out this unresolved durability choice. The remaining qualification question is whether the Android-visible success contract is preserved; mechanical reversal to old source bytes cannot answer that question. No allowance, reverse patch, manifest repin, or runtime approval is added here.

## Reproduction

On the repaired checkout:

```sh
python3 tools/test_arm64_pending_rollback.py
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tools/test_arm64_pending_rollback.py --sanitize
```

Run the unchanged command in a separate checkout of `4b30aac29c5e88dcc5841769544d52acbd136d46` to reproduce the original compiler failure. The archive's `reproduction/native_pr432_pending_replay.py` invokes that same maintained entry while retaining only its normally temporary generated directory; it does not replace extraction, compilation, or execution. Supply `--repo CHECKOUT --output NEW_DIRECTORY`, adding `--sanitize` for the sanitizer entry. The adapter control script accepts `--repo CHECKOUT --generated RETAINED_DIRECTORY/generated/test.cc --output NEW_DIRECTORY` and compiles the three explicitly described controls from that complete actual translation unit.

To observe the separate source rejection:

```sh
python3 tools/net-tcp-priority/test-selected-policy.py
```

That failure remains expected for the author source until its protected runtime changes are independently resolved and qualified.

## Evidence inventory

The companion JSON contains source identities, exact failing jobs and scope. `docs/research/evidence/arm64-pending-fixture-2026-11-23.tar.gz` contains 51 regular members (399,870 compressed bytes), SHA-256 `86b08dd5255f921f5c2f46294176871d8767fc4541d18a193a51e391146872dc`. It retains the two complete original CI logs, original and repaired generated inputs, every replay and control log, original manifests/policy, complete relevant translator/fixture headers and portable replay scripts. Each payload is hashed by `manifest.json`; executable hashes remain in the receipts, and binaries can be rebuilt from the complete generated inputs. The archive's protected-source review explicitly records that no runtime qualification was adopted.
