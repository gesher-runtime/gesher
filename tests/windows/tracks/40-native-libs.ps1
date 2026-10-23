# Phase 6: native x86-64 libraries. Dot-sourced by run-tests.ps1, so Check,
# NoDrive, $Dir, $root, $bcp and $failed are in scope.

# Can Windows user code set the %fs base (WRFSBASE), for bionic's TLS and the
# stack-protector cookie? This is the open question of ADR 0014. It is reported
# as INFO, not a pass/fail gate: a .so that does not touch %fs (like the step-1
# trivial one) loads and runs regardless, and the next session resolves %fs
# before a libc++ / stack-protected .so needs it. The probe is crash-safe and
# prints its own verdict.
$fsOut = & "$Dir/fsbase_probe.exe" 2>&1 | Out-String
if ($fsOut -match 'all passed') {
  Write-Host 'INFO  fsbase: %fs IS usable via WRFSBASE on this runner'
} else {
  Write-Host 'INFO  fsbase: %fs NOT usable via WRFSBASE here (ADR 0014: the loader rewrites %fs instead)'
}
($fsOut -split "`n" | Where-Object { $_ -match 'ok|FAIL|verdict|#UD' } | Select-Object -First 12) |
  ForEach-Object { Write-Host "      $_" }

# The ELF loader maps a self-contained x86-64 Android .so (built by the NDK in
# native-libs.yml), applies its relocations, runs its constructors and calls
# its exports. This is the step-1 gate.
Check 'ELF loader: load a trivial .so and call it' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/trivial.so")
} @('elf_load_test all passed')

# The bionic shim: a .so built against bionic's libc, libm, libdl, liblog and
# pthread (tests/native/libc_test.c) loads through the ELF loader and passes
# its own checks: memory, strings, printf/scanf through the System V vararg
# bridge, errno, time, qsort, threads, mutexes, condition variables, keys,
# dlsym and dl_iterate_phdr.
Check 'bionic shim: a libc/pthread .so runs its checks' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libc_test.so")
} @('libc_test: all passed', 'elf_load_test all passed')

# %fs without WRFSBASE (ADR 0014): NDK libraries built with the default stack
# protector and thread-locals (tests/native/fs_test.c). The loader rewrites
# their %fs accesses to %gs-based ones at load time; the libraries check the
# guard, the thread pointer and their thread-locals across sleeps and threads.
# API 26: emulated thread-locals. API 29: ELF TLS (general-dynamic,
# local-dynamic, initial-exec), and a TLS library dlopen()ed while a thread
# runs.
Check 'native libs: stack-protected .so with emulated TLS (API 26) under the %fs rewrite' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/fs_test26.so") (NoDrive "$Dir")
} @('fs_test: all passed', 'elf_load_test all passed')

Check 'native libs: stack-protected .so with ELF TLS (API 29), threads and a late TLS library' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/fs_test29.so") (NoDrive "$Dir")
} @('fs_test: all passed', 'elf_load_test all passed')
