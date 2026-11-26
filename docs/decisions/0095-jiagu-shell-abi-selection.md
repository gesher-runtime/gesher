# 0095 — 360 Jiagu shells without x86 libraries: no change

**Status:** decided not to change Gesher, 2026-11-24 (native-libraries track,
issue #426). Open scope question at the end.

Related: [0047](0047-arm64-translation.md) (ABI selection and
`Build.SUPPORTED_ABIS`), [0084](0084-native-libs-self-unpacking-code.md) and
[0086](0086-packer-art-hooks.md) (other packers' shells).

## Context

EWPE Smart 1.27.0.1 (`com.gree.ewpesmart`, APKPure XAPK: `base.apk` and
`split_config.arm64_v8a.apk`) is packed with 360 Jiagu (加固). It ships native
code only for `arm64-v8a`. In issue #426 it exits at startup:

    UnsatisfiedLinkError: dlopen /…/data/data/com.gree.ewpesmart/.jiagu/libjgcge.so: Windows error 126
      at tramsepwe.eerg.moc.a.a(SourceFile:269)          // System.load
      at tramsepwe.eerg.moc.StubApp.attachBaseContext(SourceFile:190)

Windows error 126 means the file does not exist: `dlopen` hands a file that is
not ELF (or missing) to `LoadLibraryExW`.

### What the stub does

The stub's dex (`classes.dex`: `tramsepwe.eerg.moc.{StubApp,a,DtcLoader,
Configuration}`; its strings are XORed with 0x10) was disassembled with
`dexdump` from build-tools 36.1. `StubApp.attachBaseContext` (constants from
`<clinit>`: `c = "libjgcge"`, `loadFromLib = false`, `needX86Bridge = false`):

1. `x86 = a.a()`, true when any of these holds:
   `Build.SUPPORTED_32_BIT_ABIS[i]` contains `x86`; `Build.CPU_ABI` or
   `Build.CPU_ABI2` contains `x86`; a line of `/system/build.prop` containing
   `ro.product.cpu.abi` contains `x86`; `/system/bin/ls` is an ELF file with
   `e_machine` 3 (i386) or 62 (x86-64).
2. `is64 = Build.CPU_ABI.contains("64") || Build.CPU_ABI2.contains("64")`.
3. The directory is `getFilesDir().getParentFile().getCanonicalPath() +
   "/.jiagu"`; the 32-bit target name is `libjgcge.so`, the 64-bit one
   `libjgcge_64.so`.
4. It copies the asset `libjgcge_x86.so` (x86) or `libjgcge.so` (otherwise) to
   `.jiagu/libjgcge.so` through `Context.getResources().getAssets().open()`,
   then, when `is64`, `libjgcge_x64.so` (x86) or `libjgcge_a64.so` to
   `.jiagu/libjgcge_64.so`. A failed copy returns false (exceptions are
   caught).
5. If the 64-bit copy succeeded it `System.load`s `.jiagu/libjgcge_64.so`
   (line 188), otherwise `.jiagu/libjgcge.so` (line 190).

It never reads `ApplicationInfo.primaryCpuAbi`, `nativeLibraryDir` or the
split APKs: the shell libraries are assets of `base.apk`.

`base.apk`'s assets hold `libjgcge.so` (ELF32 ARM) and `libjgcge_a64.so` (ELF64
AArch64), and `.jgapp`; there is no `libjgcge_x86.so` or `libjgcge_x64.so`.

### On Gesher

`Build.CPU_ABI` is `x86_64` (`ro.product.cpu.abi`, as on any x86-64 device;
`CPU_ABI2` is `arm64-v8a`), so `x86` and `is64` are true. Both copies fail
(`FileNotFoundException` from `AssetManager.open`), and line 190 loads
`.jiagu/libjgcge.so`, which was never written. That is the reported failure.

### On an Android 16 device with x86-64 and arm64 translation

The same. `Build.CPU_ABI` is the first entry of `ro.product.cpu.abilist64`
(`android.os.Build`'s static initialiser), `x86_64` on an x86-64 device with
NDK translation or Houdini (an x86-64 emulator image with ARM translation, a
Chromebook, Windows Subsystem for Android); its `/system/build.prop` and
`/system/bin/ls` say x86 too. The stub takes the x86 branch, finds no x86
assets and loads a file that does not exist: `dlopen failed: library … not
found`, an uncaught `UnsatisfiedLinkError` in `attachBaseContext`, and the
process dies. `needX86Bridge` (Jiagu's switch for loading the ARM shell
through the native bridge on x86) is `false` in this build: the app was
protected without Jiagu's x86 support.

## Decision

**No change to Gesher.** Every value the stub reads is what an x86-64 Android
device with arm64 translation reports, and the stub's choice depends on
nothing else (not on `primaryCpuAbi`, split APK paths or asset access, which
all work). Making it pick `libjgcge_a64.so` would mean reporting a non-x86
primary ABI (`Build.CPU_ABI`, `SUPPORTED_ABIS`, `ro.product.cpu.abi`) to this
app, which is not what Gesher is: Gesher runs x86-64 code natively and arm64
code through translation (0047). Per CONTRIBUTING.md ("Honest capabilities"),
that is not done for one packer's convenience.

The only Gesher-side difference is the error text: Android's message is
`dlopen failed: library "…" not found`, Gesher's is `Windows error 126`.
The outcome (an `UnsatisfiedLinkError`, then the process exits) is the same;
the text is handled separately from this decision.

## Consequences

- EWPE Smart 1.27.0.1 stops at the same point it would on an x86-64 Android
  device with ARM translation. A build of the app protected with Jiagu's x86
  support (`libjgcge_x86.so`/`libjgcge_x64.so` in its assets, or
  `needX86Bridge` set) would take a different path.
- Not explored, because the stub never gets there: what `libjgcge_x64.so`
  (or, through the bridge, `libjgcge_a64.so`) expects of ART. Jiagu's shell
  is known to hook ART by symbol like Ijiami's (0086); that would be the next
  step for a Jiagu build that includes x86.

## Open scope question

Whether Gesher may present itself as an arm64-only device (`CPU_ABI`
`arm64-v8a`, no x86 ABIs, matching `build.prop`) to an app that ships only
arm64 code, running all of it through translation. That is a different device
model, not a fix to a contract, and it would change what every such app sees
(not only packed ones). It needs a maintainer decision before any work.
