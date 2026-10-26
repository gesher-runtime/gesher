# Track brief: native2

Native-library and file-system leftovers reported by the arm64 and posix
tracks. The original brief given to the first agent of this track
(2026-10-26), kept verbatim. A successor reads it together with the "Status
and next steps" section in the track's doc on its branch.

You are the "native2" track agent of the gesher project. Your worktree is
C:\work\gesher-arm64 (reused) on branch wip/native2 (cut from
phase4/integration and pushed). Work only there, with absolute paths under
C:\work\gesher-arm64 (another agent once wrote into the coordinator's
worktree by mistake: check `pwd` before editing).

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/native-libs.md (the bionic shim and its gaps), docs/arm64.md
and its guest_libc.cc (the fallbacks it added for arm64 only), docs/posix.md
and ADR 0045 (its leftovers list), compat/bionic.

GOAL, each verified in CI:
- Move the arm64 track's libc/libm fallbacks (sem_*, the 32-bit wchar_t
  functions, libm extras: ~50 functions, used only when the shim lacks the
  name) into compat/bionic, so x86_64 apps get them too; arm64 then uses
  the shim's (guest_libc.cc keeps only what is arm64-specific: struct and
  va_list conversion, syscall numbers, setjmp).
- `dlopen` of platform libraries (libEGL.so, libGLESv2.so, libandroid.so,
  liblog.so, libOpenSLES.so, libaaudio.so, libz.so, libc++_shared's role,
  libvulkan.so if present, libmediandk.so, libjnigraphics.so, ...) by name
  and by /system/lib64 path, with `dlsym` on the handle and RTLD_DEFAULT,
  for x86_64 (it fails today) and arm64.
- Find the shim's remaining gaps the way Android apps hit them: run the
  NDK libraries of the e2e apps and a set of popular NDK libraries'
  self-tests (your choice; e.g. libpng, libjpeg-turbo, sqlite, openssl's
  `openssl speed` or BoringSSL's tests, SDL's testautomation, libc++'s
  basic tests) against the shim and close what is missing.
- posix leftovers: `stat`'s st_mode shows the execute bit where
  `access(X_OK)` holds; symlinks (Windows symlinks and junctions, `readlink`,
  `lstat`, `symlink` where the account may create them, and ART's
  DexFileLoader resolving them); `flock` and ART's ScopedFlock (LockFileEx);
  inotify's IN_MODIFY reported on write, not only at close, where NTFS
  allows.
Tests in tests/windows/tracks/44-native2.ps1 (NDK test programs and
libraries of your own), both runners, single process and on the Gesher
device; 40-native-libs, 21-posix and 47-arm64 must stay green; e2e too.
Your file domain: compat/bionic (shared with exec2, which adds process and
signal functions there right now: minimal edits, merge origin/wip/exec2's
changes only through phase4/integration), the arm64 guest_libc.cc, compat/posix
file-system code (shared with exec2: careful), the ART patches you need
(new numbers after the existing ones in patches/art), tests/windows/tracks/44-native2.ps1
and its test programs, docs/native-libs.md and docs/posix.md sections, ADR
docs/decisions/0057-native2-shim-and-filesystem.md (0057 is reserved for
you), the roadmap boxes.

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (media, windows3, aot2, exec2, surfaces, web3):
trigger CI only when code is ready, cancel superseded runs. Watch with `gh
run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`; on
HTTP 403 back off ~10 minutes. Keep a "Status and next steps" section
updated with every meaningful push. Before your final CI run, merge
origin/phase4/integration into your branch. Final message = the report
track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside
your domain, the verified SHA).
