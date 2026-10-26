# Compiled code: nterp, AOT and the JIT

Until the codegen track, every Gesher process ran ART's switch interpreter
only (`-Xint`): the boot image had no code, the JIT was off and nterp (ART's
assembly interpreter) was disabled, because ART's x86-64 compiled code and
assembly reach the current `Thread` through `%gs`, which on Windows is the
TEB. This brings the three faster execution modes to Windows. The design is
[ADR 0033](decisions/0033-codegen-compiled-code-on-windows.md); it extends
[ADR 0006](decisions/0006-managed-abi.md).

## What changed
- **`Thread*` at a constant TEB offset.** gesher-base takes TLS slot 63 at
  startup, so `Thread*` is always at `%gs:0x1678`, which compiled code reads
  with one instruction (ADR 0033; `compat/posix/src/thread_register.cc`).
- **The x86-64 code generator** reaches every Thread field through that slot
  instead of `%gs:offset` (ART patch 0015), as do the oat trampolines and the
  math entrypoints (System V wrappers).
- **Faults become signals** and thread stacks grow for stack probes
  (`compat/posix/src/fault_signals.cc`): implicit null checks and the stack
  overflow probe that nterp and compiled code rely on now work.
- **nterp** is on (implicit null and stack overflow checks on, patch 0014).
- **The boot image** carries AOT-compiled code for its primary image and its
  extensions (`--compiler-filter=speed` for core-oj, core-libart, core-icu4j,
  framework.jar and gesher-app.jar; `tools/boot-image/compile.ps1`,
  `boot-image.yml`).
- **The JIT** is linked into dalvikvm (libart-compiler) and compiles app code;
  its code cache is dual-mapped (W^X): code is written through a read-write
  view of a memfd and runs from a separate read-execute view (ADR 0036).
- **Deep recursion** throws `StackOverflowError` under every execution mode:
  Windows' `EXCEPTION_STACK_OVERFLOW` at a stack probe is delivered as the
  `SIGSEGV` ART expects, and ART's protected region sits right above the
  stack guarantee (ADR 0036; `fault_signals.cc`, `pthread_attr_getguardsize`).
- **Compiled JNI stubs**: the JNI macro assembler and the JNI trampolines reach
  `Thread*` through the TLS slot (ART patch 0016); the boot image has AOT JNI
  stubs, the JIT compiles the others.
- **Oat files are mapped, not copied**: gesher-base's `mmap` fills placeholder
  reservations with views (`MapViewOfFile3`), executable ones included, so an
  oat file's code is a view of the file shared between processes (ADR 0036).
- **Android's compilation model** (ADR 0054, below): the boot image is
  compiled `speed-profile` with AOSP's boot image profiles; the Gesher device
  compiles an app at install (`verify`), runs its processes with the JIT and
  its profile saver, and compiles it again in the background with its profile
  (`speed-profile` and an app image).

## Diagnosis switches
- `GESHER_ART_INTERPRET_ONLY=1` — the switch interpreter only, explicit checks,
  no nterp, no compiled code, no JIT (the configuration before this work).
- `GESHER_ART_NO_AOT=1` — ignore the boot image's compiled code (nterp, JIT).
- `GESHER_LOG_EXECUTION=1` — log the execution mode and the boot image's
  compiled-method count.
- `GESHER_TRACE_SIGNALS=1` — log the first 200 faults and what became of each.
- `GESHER_TRACE_MMAP=1` — log every `mmap` call, and for file mappings whether
  they became a view (`an executable view of ...`) or a copy.
- `GESHER_MMAP_NO_PLACEHOLDERS=1` — plain allocations and views, oat code
  copied, no dual-mapped JIT cache (the configuration before ADR 0036).
- `GESHER_LOG_EXECUTION=1` also logs, for an app, the oat file its dex file
  gets (`gesher: app oat: <apk>: <odex> (status, filter, reason, executable)`),
  its app image (`gesher: app image loaded: <art>: N classes, M methods, K with
  compiled code`) and, at `System.exit`, how its own methods ran (`gesher: app
  methods at exit: ... with AOT code, ... entered the interpreter or the JIT`).
- `GESHER_APP_EXECUTION=interpret` (in the device's environment) — app
  processes keep the device's interpreter-only options (before ADR 0054).

## Status
Verified in CI (`tests/windows/tracks/70-codegen.ps1`, run-tests.ps1 sets
`GESHER_REQUIRE_BOOT_IMAGE`), on windows-2022 and windows-2025:

- [x] **nterp** runs Hello, the stress test and the benchmark; it matches the
      switch interpreter's benchmark checksum.
- [x] **The JIT** compiles app code (its code cache fills) and matches the
      same checksum.
- [x] **AOT**: the boot image's primary image is compiled with `speed` (53,927
      methods with compiled code in CI); the nterp and JIT modes run app code on
      nterp / the JIT while the boot library (String, Math, collections, ...)
      runs its AOT code.
- [x] **Stress** (implicit null checks, exceptions through compiled frames,
      GC with compiled frames on the stack, threads and monitors, JNI from
      compiled code, proxies, reflection, array/arithmetic/cast exceptions)
      passes under the switch interpreter, nterp (over AOT) and the JIT (over
      AOT), and under the interpret-only diagnosis mode.
- [x] **ART's own run-tests** (a subset of `art/test` that needs no native
      library): 008-exceptions, 021-string2, 046-reflect, 080-oom-throw,
      083-compiler-regressions, 114-ParallelGC, 123-compiler-regressions-mt —
      the same stdout under the switch interpreter, nterp and the JIT.
      (003-omnibus-opcodes, 061-out-of-memory and
      201-built-in-except-detail-messages do not pass under the switch
      interpreter on Gesher either, for reasons unrelated to compiled code;
      they are reported, not counted.)

### Benchmark (`gesher.Bench`, best of 3 rounds, ms; windows-2025, run 40068699984)
A CPU-bound dex benchmark; app code under the switch interpreter vs nterp vs
the JIT (the boot library runs AOT code in the nterp and JIT columns):

| kernel | switch | nterp | JIT |
|---|---|---|---|
| fib | 20 | 4 | 0 |
| int-loops | 149 | 37 | 3 |
| sieve | 42 | 10 | 1 |
| sort | 150 | 20 | 7 |
| virtual-calls | 125 | 33 | 1 |
| allocation | 730 | 149 | 141 |
| strings | 163 | 6 | 6 |
| floats | 95 | 44 | 3 |
| **total** | **1474** | **303** | **162** |

nterp is about 5x the switch interpreter on this benchmark's total, the JIT
about 9x (the full process wall time, which includes startup, 4791 -> 1233 ->
864 ms). The checksum is identical in all three modes.

## Status and next steps (stack track, ADR 0036)
Verified in CI on windows-2022 and windows-2025 plus package and e2e (ci run
40134720336, 23bd046; again in 40138006773):
- [x] Deep recursion: `StackOverflowError` caught and recovered from under
      switch, interpret-only, nterp and the JIT (app, boot AOT and native-call
      recursion, 4 rounds, main thread, new threads, two at once).
- [x] Compiled JNI stubs: 844 AOT JNI stubs in the boot image; the JIT
      compiles app natives' stubs (`gesher.JniStubs`).
- [x] Placeholder mappings and executable oat views (`mman_probe`, 26
      checks; 60-boot-image: oat code mapped as views, none copied).
- [x] Dual-mapped (W^X) JIT code cache.
- [x] Math entrypoints against StrictMath in the stress test, every mode.

Verified in ci run 40138006773 (1c158b4, both runners, package, e2e):
- [x] AOT for the extensions (boot-image.yml `-ExtensionFilter speed`):
      298,365 methods with compiled code with framework.jar and gesher-app.jar
      on the boot class path (2,581 JNI stubs), 54,771 with the primary image
      alone; the oat code is 5 executable views, 74.9 MB, none copied.
      dex2oat: primary 2.7 s, framework.jar 8.8 s, gesher-app.jar 0.7 s;
      boot-framework.oat is 75.7 MB.
- [x] Without placeholder mappings (`GESHER_MMAP_NO_PLACEHOLDERS=1`) the JIT
      takes its single mapping (patch 0017's `memfd_create` fails with
      ENOSYS) and the stress test passes.
- Startup with the compiled extensions (60-boot-image, median of 3):
  lifecycle.apk to `Activity.onCreate` 312 / 340 ms with the image, 744 /
  745 ms imageless (windows-2022 / 2025); before, with verify-only
  extensions (run 40134720336, windows-2022), 254 ms vs 541 ms imageless.
  Those runners were faster overall (benchmark totals 871 vs 1350 ms under
  the switch interpreter), so the ratio to imageless is the fairer
  comparison: 0.47 before, 0.42 / 0.46 now - no measurable startup cost from
  the larger oat files, which are mapped, not read.

Next (not part of this track's goal): an app image for the app's own
classes; a JIT profile / `speed-profile` for framework.jar to shrink its
75 MB oat file. (Done by the aot2 track, below.)

## Apps and the framework as Android compiles them (aot2 track, ADR 0054)

### The boot image with a profile
`boot-image.yml` fetches AOSP's boot image profiles in text form at the
pinned tag (`art/build/boot/boot-image-profile.txt`, 14,017 lines, and
`frameworks/base/boot/boot-image-profile.txt`, 38,909 lines;
`tools/boot-image/fetch-profiles.ps1`); profman.exe makes one binary boot
profile of them (`--output-profile-type=boot`, as Soong does); dex2oat
compiles the primary image and framework.jar `speed-profile`: only the
profile's hot and startup methods get AOT code, and only its classes go into
the image. gesher-app.jar, which no AOSP profile covers, stays `speed`.

### Apps
On the Gesher device (`com.gesher.server.Dexopt`):

| when | what | filter | files |
|---|---|---|---|
| install | dex2oat (`pm.dexopt.install`, no profile yet) | `verify` | `<code path>/oat/x86_64/base.{odex,vdex}` |
| every run | the app process runs nterp and the JIT with `-Xjitsaveprofilinginfo`; ART's profile saver writes the app's profile, at the latest when it exits | | `misc/profiles/cur/0/<package>/primary.prof` |
| device idle (2 s without an app process) | profman `--force-merge-and-analyze` into the reference profile, then dex2oat (`pm.dexopt.bg-dexopt`) | `speed-profile` | `base.{odex,vdex,art}` (`art`: the app image), `misc/profiles/ref/<package>/primary.prof` |

ART's OatFileAssistant finds the oat file next to the APK; with an app image
the app's profiled classes are already loaded, linked and initialized when
it starts, and its startup methods run their AOT code.

## Status and next steps (aot2 track, ADR 0054)
Verified in CI on windows-2022 and windows-2025 (`tracks/61-aot2.ps1`), plus
package and e2e:

- [x] **Boot image with AOSP's profiles** (ci 40237467952, 40240540799,
      40242793689): boot-framework.oat 9.1 MB (was 72.2 MB with `speed`, the
      same jars compiled by the test), all `.oat` files 16.5 MB (was 96.3 MB),
      the whole image 74.5 MB (was 162.4 MB); `.art` files 7.1 MB (was 14 MB).
      dex2oat: primary 1.6 s, framework.jar 3.1 s.
- [x] **Startup no worse** (61-aot2, median of 3, lifecycle.apk to
      `Activity.onCreate`, the profile image vs the `speed` image on the same
      runner, ci 40242793689): interpreter 503 / 499 ms (windows-2022), 618 /
      648 ms (windows-2025); JIT 366 / 385 ms, 480 / 486 ms.
- [x] **The installer** ships the smaller image, dex2oat.exe and profman.exe:
      88.9 MB, was 112.5 MB (ci 40237467952); the installed device compiles
      lifecycle.apk at install (package job, ci 40240540799).
- [x] **Install-time dexopt** (verify) on the device and by hand; the app runs
      from its oat file (`gesher: app oat: ... filter verify, reason install,
      executable yes`), app processes on the JIT (ci 40240540799).
- [x] **JIT profiles**: the profile saver writes the app's profile, in one
      process and on the device (ci 40242793689; file locks on Windows, patch
      0540).
- [x] e2e (8 F-Droid apps) and every other device check pass with app
      processes on nterp and the JIT (ci 40237467952 and later).
- [x] **Background dexopt** on the device (profman `--force-merge-and-analyze`,
      dex2oat `speed-profile` with `--app-image-file`) once the device is idle,
      and the next run uses the speed-profile code: the app's profiled methods
      run their AOT code and none of them entered the interpreter (8 of 16
      methods with AOT code, 0 interpreted, was 8 interpreted with verify;
      ci 40244966896, 40263271836, both runners).
- [ ] **The app image holding the app's classes** (not verified): until
      ad9d360 the app images loaded but held only the `R` classes;
      LifecycleActivity and LifecycleApplication were "unsuitable" because
      dex2oat compiled the app against the primary boot image only (patch
      0012's lone-location expansion to `<dir>/*` is runtime-only). ad9d360
      passes `--boot-image=<dir>/boot.art:<dir>/*` (Dexopt and 61-aot2).

### Handoff (2026-10-26, stopped by the coordinator)
- Branch `wip/aot2`. Last verified: 7de81ac, ci 40263271836. Everything
  passed on both runners, plus package and e2e, except the two app-image
  checks of 61-aot2 ("lifecycle.apk from its app image",
  "second launch on the device (app image)").
- In progress when stopped: ci 40285423159 at ad9d360 (the `<dir>/*` fix). It
  was not watched to the end. Check its 61-aot2 lines first.
- Next steps: (1) if 40285423159 shows `gesher: app image loaded: ...: N
  classes` with N >= 2 and compiled methods, tick the box above and the
  roadmap. If not, read the `Gesher: ... cannot go into the image` lines that
  61-aot2 prints (`-verbose:compiler`, patch 0540). (2) Decide whether to keep
  `-AllClasses` (boot-image.yml). It was added when the cause was believed to
  be a pruned boot image. It costs 6.6 MB of `.art`; without it, startup
  under the interpreter was within noise. Try without it once the app image
  works. (3) Roadmap boxes for this track are not ticked yet. (4) Merge
  origin/phase4/integration again before the final run.
- Pitfalls: `gh workflow run ci` now fails with HTTP 422 (another workflow
  is also named ci); use `gh workflow run ci.yml`. Read the device's logs with
  shared access (other processes append to them). Python string literals with
  Windows paths corrupt YAML (`\v`, `\t`). The media check on windows-2022 and
  the webapp pixel check on windows-2025 failed once each in earlier runs;
  both passed in 40263271836.
- scoped_flock (for native2, ADR 0057): patch 0540 implements
  `LockedFile::Open`/`DupOf` on Windows (`#if defined(_WIN32) &&
  defined(GESHER)`) with `LockFileEx` (exclusive, whole file,
  `LOCKFILE_FAIL_IMMEDIATELY` when not blocking) on `_get_osfhandle(fd)`. The
  lock is released when the handle closes; `ReleaseLock` is still a no-op on
  Windows. These are mandatory locks: every user takes the lock before it
  reads or writes. That covers what the profile saver and profman need, and
  nothing else.
