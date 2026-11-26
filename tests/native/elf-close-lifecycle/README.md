# Ordinary ELF close lifecycle

This standalone [N06](https://github.com/gesher-runtime/gesher/issues/56)
probe checks the public loader with three fixture DSOs. An independently
opened trace library records constructor and destructor events from a parent
library and its dependency.

The probe opens the parent twice, checks that the first close keeps both
libraries live, then checks finalizer order and `RTLD_NOLOAD` after the last
close. Reopening the graph checks fresh initialization and state. An
independent open of the dependency keeps it live until its own last close.
Each successful `RTLD_NOLOAD` lookup is balanced with a close.

## Build and run

On Linux:

```sh
bash tests/native/elf-close-lifecycle/build.sh cc "$PWD/out/elf-close-lifecycle"
out/elf-close-lifecycle/elf-close-lifecycle "$PWD/out/elf-close-lifecycle"
```

For Android:

```sh
out="$PWD/out/elf-close-lifecycle-android"
guest=/data/local/tmp/gesher-elf-close-lifecycle
bash tests/native/elf-close-lifecycle/build.sh "$NDK_CLANG" "$out"
adb shell mkdir -p "$guest"
adb push "$out/." "$guest/"
adb shell chmod +x "$guest/elf-close-lifecycle"
adb shell "$guest/elf-close-lifecycle" "$guest"
```

Set `NDK_CLANG` to an ABI-specific NDK driver such as
`aarch64-linux-android28-clang`. Use an Android debug shell to execute the
fixture. To check the same guest executable with a Gesher build, copy the
output directory to Windows and use the ABI-compatible runtime:

```powershell
$dir = 'C:\Path\To\elf-close-lifecycle-android'
.\gesher-exec.exe --elf (Join-Path $dir 'elf-close-lifecycle') elf-close-lifecycle $dir
```

The oracle exits zero when all 20 checks pass and one when any check fails.
A loader or fixture setup failure exits two.

## Reference and remaining work

Android 16 ARM64 and Ubuntu 24.04 ARM64 each passed 20/20. Bionic decrements the
load group's reference count, runs finalizers when the group retires, and
then removes its mappings. [Android 16 linker](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/linker/linker.cpp#1892)

Windows x64 and ARM64 production ELF components each passed 10/20. x64 ran
under Windows on ARM emulation. Both used the same fixture cases with the
production ELF loader and the shim's public open, symbol, close and error
handlers. The component harness supplied native callback entry points for
the test process. It did not run a packaged ART runtime.

Gesher reported success from each close. The trace contained only the two
initializations. Both objects stayed discoverable through `RTLD_NOLOAD`
and retained the previous dependency data when reopened. Running finalizers
in place would still leave old relocations and data visible to a subsequent
`dlopen`. The lifetime implementation must account for public and ART
handles, the `DT_NEEDED` graph, and bindings through the existing relocation
fallback. TLS, reserved mappings and active loader callbacks require their
own retirement handling. N06 remains open.
