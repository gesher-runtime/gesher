# Process environment ownership

The host POSIX layer and loaded Android libraries share one process environment.
The stable `environ` and `__environ` exports identify its table slot, so growing
the pointer array does not leave an already relocated ELF library with an old
array. The executable owns the state; linked modules resolve the same owner.

## Storage and mutation

`setenv` owns a copy of the assignment. Replaced owned strings remain readable
for callers which retained a previous `getenv` result. `putenv` retains the
caller's buffer: later changes to that buffer's name or value are visible to
environment lookup and process inheritance. It must replace the pointer even
when the old and new bytes compare equal. A following `setenv` detaches the
entry from borrowed storage, including an equal-value assignment.

The caller remains responsible for a borrowed buffer's lifetime. The environment
owner never gives that buffer to UCRT, which owns and frees its own copied
environment strings. The routed `_putenv`, `_putenv_s` and `_wputenv_s` entry
points update copied shadows and the shared POSIX owner. POSIX names remain
case-sensitive; native control-key lookup and setters also recognize ASCII
case aliases. Raw Windows environment APIs and CRT shadow readers are separate
interfaces, not additional intercepted POSIX entry points.

An empty value is a present POSIX assignment. A `putenv` argument without `=`
removes the named entry. Preparation failures and failed native shadow writes
do not publish a new POSIX table or assignment. Reads do not copy the complete
table; pointer-array capacity grows geometrically, and process launch takes a
snapshot only when it needs one.

## Process and font boundaries

An explicit `envp` uses the caller's assignments; default inheritance snapshots
the shared owner. Native default inheritance also retains Windows' private
drive entries. The environment transport preserves UTF8 and inherited unpaired UTF16 surrogates;
it rejects malformed byte sequences instead of substituting a different value.
The ordinary path and command-line converters are separate from this transport.

The two native font keys keep their existing case-alias, last-writer and empty
value policies. Ordinary owned font writes keep the immutable snapshot path.
Borrowed font values are copied only when the font reader needs a stable view
of caller-mutable bytes. Font validation still rejects invalid Unicode and
does not turn an invalid configured root into a default path.

The environment writer and snapshot locks are separate. Native CRT calls do
not retain the snapshot lock, allowing callbacks to read the prior committed
state. Inputs whose lengths UCRT rejects before mutation enter that original
CRT primitive without either owner lock; an existing value with `overwrite=0`
still skips the write. This preserves the invalid-parameter callback's original
lock boundary. The font fixture's default strict mode requires an actual wait
chain through the held CRT critical section, followed by successful callback
and writer completion; a different wait or a timeout is not a pass.

Its explicit `--functional` mode has a narrower scope: initial and owned font
values, borrowed-buffer changes, the pre-mutation invalid-handler read callback,
and concurrent snapshot reads. It does not run the held-writer callback test and
reports `held_writer_callback_qualified=0` and `crt_owner_qualified=0` in a separate
result marker. A functional result cannot satisfy the strict result parser.

## Regression evidence

- `tools/test_environment_contracts.py` executes the production owner and table
  in an explicit Linux envelope. It covers ownership, mutation, allocation and
  error handling, snapshot lock order and environment encoding. Its optional
  historical shim input isolates the copying `putenv` regression.
- The Android `environment_lifetime_test.so` exercises the public ELF imports
  and retained strings in the normal native suite.
- The exec4 host and ELF fixtures exercise relocation before table growth,
  native narrow/wide writes, mutable and empty assignments, `posix_spawn` and
  a real child `execv` without an explicit environment argument.
- The ordinary Windows lifecycle suite builds `font_environment_test` and runs
  the functional scope in all five initial UTF16 modes on Windows 2022 and 2025.
  The CI runner explicitly selects the additional unchanged strict five-mode
  wait-chain gate on Windows 2022. The authenticated runtime artifact supplies
  that executable. Missing strict owner evidence is never accepted as a strict
  pass, and the older font component's frozen supplier qualification is unchanged.

Windows 2025 held-writer/reentrant-reader and full WCT owner qualification remain
open. That limitation predates this environment owner; the existing
[font resource handoff](../handoff/tracks/fonts-resource-parser-source.md) retains
the historical five Windows 2025 WCT failures. Ordinary functional results do not
replace those failures or the separate owner investigation.

Linux envelopes and cross-compilation establish only their stated boundaries.
The native ELF, process-inheritance and selected five-mode font gates must execute
on the composed Windows build before their respective runtime results can be
claimed. The functional gate alone does not establish progress for a reentrant
read while another writer is blocked inside the CRT.
