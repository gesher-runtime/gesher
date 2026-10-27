# ARM full environment/default object: implementation awaiting CI

Resume implementation: the separate environment worktree now contains real
FPCR/FPSR get/set/hold/update and an ARM-specific eight-byte default data
object. Shared ELF and Native2 JNI/single/device fixtures compare the
unchanged pinned bionic body across256 control/status cases for direct and
dynamic entries, output canaries, default-object identity/bytes, errno,
concurrent guest threads and nested guest-host-guest callbacks. Reserved
test85 requires actual process reports. A guarded environment component
retains all existing coherent API3/Native2/FPCR/FPSR/varargs/JNI controls and
the exact five-jar/image source checks. Source awaits bounded peer review
and CI; no compiler or fixture ran locally, no acceptance/roadmap closure.

The earlier audit and proposed scope below are historical context; only
the source/CI results subsequently recorded here can establish completion.

Owner requested a nearest-checkpoint stop on2026-10-27. This is a docs-only
WIP, not an implementation or verification claim. Separate worktree
C:\work\gesher-arm64-env, branch wip/arm64-env, starts at coherent
API3/Locale/FPSR candidate0cfcd2e0ad594fc85936de209053dc21f3463669.
Reserved Windows test85 remains under existing ADR0067. No frozen Locale2,
Wide, FPCR or FPSR ref was changed. Every compiler and executable fixture
belongs in CI; none ran locally.

## Exact source audit

Read the complete already-vendored, licensed pinned
probes/arm64/fpcr/upstream/fenv-arm64.c and fenv_arm.h, their sources.json,
verify-pinned.py and build-reference.sh. The pinned Android16 r4 source digest
is09612eff991d928627b7a543488d64edd557fedbc8a21d947e1261bcaf4ce67d;
the ARM header digest is
0e2bddb41d64a1b8e1155c0702bf48c193845a079d5e57a62f03f1f80137591b.
Existing CI builds the original body with renamed symbols, including
arm64_reference_default and the full environment entries; do not replace
that independent producer with a locally mirrored implementation.

Pinned ARM LP64 fenv_t is exactly two uint32_t fields: control/FPCR then
status/FPSR (8 bytes); fexcept_t is4 bytes. __fe_dfl_env is the actual const
two-field object{0,0}. fegetenv snapshots both real registers; fesetenv writes
the supplied control/status. feholdexcept snapshots then clears only
FE_ALL_EXCEPT0x9f, preserving QC and control. feupdateenv captures current
exception bits, installs the supplied environment, then merges those bits
into its status. The pinned source explicitly leaves traps unsupported:
feenableexcept returns-1, fedisableexcept0 and fegetexcept0. feholdexcept
does not implement independent trap masking beyond the actual pinned body.

Current compat/arm64/dll/guest_libc.cc still zeroes both output fields in
FeGetenv, makes FeSetenv a no-op, maps feholdexcept to FeGetenv and
feupdateenv to FeSetenv. No ARM-specific __fe_dfl_env override is installed.
The common x86 shim exposes its own X64Fenv object; guest imports/dlsym must
resolve the correct ARM data object rather than that layout. GuestSymbol,
GuestDlsym and InitGuestLibc were read for that data/function distinction.
Existing GuestFpcr/SetGuestFpcr/GuestFpsr/SetGuestFpsr already address actual
per-thread/per-nesting dynarmic state and are the proposed narrow boundary.

## Proposed next source, requiring bounded review before CI

- Add an explicit8-byte ARM environment structure/const default object in
  guest_libc.cc. Install the data object directly in g_overrides; function
  wrappers continue through the existing host binding path. Keep the common
  x86 object and APIs unchanged.
- Implement only pinned get/set/hold/update semantics through real current
  guest FPCR/FPSR, preserving FE_ALL_EXCEPT0x9f/QC behavior and the existing
  host fenv crossing guards. Do not invent null-pointer behavior beyond the
  valid-pointer pinned contract or new privileged register support.
- Share a new real guest contract header across an actual ELF caller and
  Native2 JNI/single/device APK contexts. Compare production and the unchanged
  renamed pinned bodies, with8-byte output canaries, direct imported and
  dlsym/default-object identity/bytes, get/set/hold/update, all modes and
  supported DN/FZ/status/QC combinations, errno, threads and nested callbacks.
  Preserve every existing FPCR69/FPSR75 and independent10384-row oracle check.
- Add reserved test85 and precise CI build/runtime wiring only after the
  source delta is reviewable. Send the exact source to Root/Media for bounded
  peer review before CI. First consume the verified coherent API3 prerequisite
  rather than altering its frozen leaves.

No production/test source change, env85 CI dispatch, compiler or fixture
execution exists in this checkpoint. No roadmap item is ticked. Actual
hardware traps, host CRT/libm rounding/status, full binary128, direct ARM
variadics/HFA, guest JIT/signal-context/LSE and cache/stack limits remain open;
this proposed slice cannot close those independent contracts.
