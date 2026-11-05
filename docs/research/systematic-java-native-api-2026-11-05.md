# SDK36 Java class and API36 native export inventories

These inventories find gaps before an app happens to exercise them. They
measure class definitions and export-name lookup separately from behavior,
callbacks, graphics formats, hardware and full Android compatibility.

## Java: actual boot DEX definitions against SDK36

`tools/java_api_inventory.py` compares the local SDK36 android.jar class
definitions with all DEX class definitions in the five actual installed boot
JARs: core-oj, core-libart, core-icu4j, framework and gesher-app. It does not
mistake referenced DEX types for definitions. A candidate comparison replaces
only gesher-app.jar with `out/stability/java-candidate/gesher-app.jar`.

| Input | SDK class definitions | Defined on selected boot path | Absent from selected boot path |
| --- | ---: | ---: | ---: |
| Installed | 6,227 | 5,245 | 982 |
| Candidate app replacement | 6,227 | 5,250 | 977 |

The five newly available public classes are ScanCallback, ScanRecord,
ScanResult, TransportBlock and TransportDiscoveryData under android.bluetooth.le.
Presence does not establish Bluetooth hardware support or each method's
behavior. Candidate synthetic/anonymous class churn is retained in the raw
delta rather than presented as a feature count.

The main absent boot families are connectivity/network-related classes (285),
other framework classes (230, including AppSearch), Health Connect (207),
AdServices (154), Bluetooth (61), NFC (33) and SDK sandbox (10). Two additional
absent types are AndroidX annotations. Core java, javax and dalvik public SDK
classes are present in this selected class-definition comparison. This says
nothing about their methods' signatures or implementations.

Absent from boot does not mean absent globally: module, uses-library and APK
providers were not searched. The family labels are ownership hints, not proof
of a provider's availability. Only ScanCallback and WifiManager from the
absent boot set matched missing-class observations in the selected log audit;
the rest are static availability findings, not observed app failures.

The parser initially rejected real boot DEX files because it assumed class
definitions must follow type-index order. It now accepts superclass/interface
dependency order and rejects duplicate definitions, following the official
[DEX format](https://source.android.com/docs/core/runtime/dex-format).
Six synthetic tests cover dependency order, definitions versus references,
duplicates, checksums, truncated headers, multidex unions and unsafe members.
All five real boot JARs and the candidate parse successfully.

The exact SDK/JAR hashes, annotation flags, class rows and candidate delta are
in `out/compat-audit/java/java-class-availability.json`; its Markdown companion
contains the raw class list. SDK android.jar SHA-256 is
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
The candidate app JAR SHA-256 is
`d4cf0d15860011ab2b85917827eb18303ccc587923f43c72596027d9d4b57254`.

## Native: fresh public API36 reference and actual runtime lookup

The reference was freshly downloaded through the authorized ssh/scp host.
The full official NDK r30 Linux archive, 738,633,529 bytes, passed the
[published SHA1](https://developer.android.com/ndk/downloads)
`5107f898313790e449e87eee2183d9a20602dee9`. source.properties identifies
30.0.16248370/r30; meta/platforms.json includes API36 within its range. Only
the x86_64 and arm64-v8a API36 stub files and metadata were copied back.
Each copied file has a SHA-256 in the reference manifest.

There are 52 reference .so-named files. Two libc++.so files are linker scripts,
not ELF export stubs, and are explicitly excluded. The remaining 50
library/ABI contexts produce 4,720 (library, symbol) rows per ABI. Those rows
can overlap across libraries and are not a count of distinct APIs or defects.

`tools/native_api_inventory.py` runs an ELF lookup fixture for each ABI in a
separate interpreter-mode VM with workspace-only data and GESHER_BINDER
removed. It calls the lookup and file-I/O mechanisms but never invokes a
resolved API function pointer. Both runs exit 0: each finds 2,774 export rows
and lacks 1,946 lookups. The full per-library table is in
`out/compat-audit/native-api36/native-api36-presence.md`.

| Family | Checked per ABI | Missing lookups per ABI | Meaningful follow-up |
| --- | ---: | ---: | --- |
| libc / libm | 1,441 / 286 | 437 / 133 | Separate ordinary C/POSIX/math contracts from process/Unix-only and unwind/launcher scope. |
| libandroid | 401 | 156 | Native platform families, including configuration, headroom and newer display/input APIs. |
| libmediandk / libaaudio | 364 / 78 | 197 / 14 | Capability, format and newer audio APIs need contract-specific prioritization. |
| libjnigraphics / libnativewindow | 39 / 31 | 33 / 16 | Object shapes, bitmap formats and window/buffer ownership need behavioral probes. |
| libbinder_ndk | 150 | 150 | NDK Binder API is distinct from Java Binder support. |
| libcamera2ndk | 72 | 72 | Native Camera2 is distinct from existing Java/Windows camera paths. |
| libicu | 306 | 306 | Public NDK ICU symbol names are distinct from Java ICU availability. |
| libvulkan | 251 | 251 | Vulkan is distinct from the ANGLE/OpenGL path. |
| libEGL / libGLESv2 / libGLESv3 | 73 / 204 / 418 | 0 / 0 / 0 | Resolved names do not validate GPU behavior, extensions or shader formats. |
| libOpenSLES | 56 | 0 | Presence only; interfaces and callback semantics remain separate. |

MIDI, Neural Networks, OpenMAX AL, libsync and other library rows are included
in the machine report, including whether the library handle opened. The
`__libc_init` row carries an executable-launcher scope note; shared-VM lookup
absence alone must not be called an executable-startup failure. Symbol binding,
type and versioned name are preserved, but dlsym name presence does not
validate versioned resolution. Three export-parser tests pass.

The runtime tested is the rebuilt candidate dalvikvm.exe, SHA-256
`31423e063a16d760ed47dddb0655dad282f0befe6ff5130f84faeb43b35a6342`,
with caller-supplied build revision 564118b1. The ARM64 fixture's actual loaded
module list verified the adjacent fresh gesher_arm64.dll, SHA-256
`bc4e4dc0e116c9b551dd1ca083fd827fe5e300c2d6032219988e712b96227589`.
This is an ARM64 ELF guest on Windows x86_64, not a Windows ARM64 host build.
The report also hashes the boot JARs, probe sources/binaries, D8 and readelf.
No explicit boot image was supplied in the final run; VM defaults apply,
with -Xint/-Xusejit:false and the recorded boot JARs.

RTLD_DEFAULT and platform-handle lookup can share Gesher's global resolver.
Thus even a successful handle lookup does not prove per-library namespace or
ABI signature correctness. Missing rows across both ABIs must not be summed
and presented as twice as many independent bugs. These results complement the
Binder/JNI contract audit; they do not replace its callback and default-policy
analysis. No installed app, account data, shared server or debugger was used.

## Reproduction

```powershell
python -m unittest discover -s tools/tests -p 'test_*api_inventory.py'
python tools/native_api_inventory.py `
  --reference-dir out/compat-audit/reference `
  --runtime out/native-build/build/dalvikvm.exe `
  --boot-dir "$env:LOCALAPPDATA/Programs/Gesher" `
  --ndk-bin 'C:/Users/otzaria/tools/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin' `
  --runtime-source-revision 564118b1
```

The native tool accepts --java, --javac and --d8-jar for reproducibility;
automatic D8 discovery selected local build-tools 36.1.0 for the final run.
