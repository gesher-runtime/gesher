# ELF SONAME compatibility across target SDKs

Bionic uses the canonical basename as the SONAME for a non-executable shared
object lacking a SONAME when the application's target SDK is below 23. Modern
targets identify that object by its opened file. See the [Android 16 linker
source](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/linker/linker.cpp#3331).

The oracle loads an ordinary shared library with no DT_SONAME by a temporary
filename and restores its original filename while the object is resident.
RTLD_NOLOAD by the existing explicit path reuses that object. After the rename
only target 22 accepts RTLD_NOLOAD by the removed bare name. Target 23 and the
unset/current target both require actual file or SONAME lookup. The oracle
also checks that aliases share the constructor's state and that closing the
last reference retires the file identity.

Build and run in a dedicated local fixture directory, with the Android NDK's
compiler for the reference device's ABI. The executable uses the same Bionic
target-SDK setter as ART to test all three targets in one shell process.

```sh
aarch64-linux-android35-clang -fPIC -shared -nostdlib library.c -o n03_without_soname.so
aarch64-linux-android35-clang oracle.c -ldl -o soname_oracle
adb shell mkdir -p /data/local/tmp/gesher-soname-fixture
adb push n03_without_soname.so soname_oracle /data/local/tmp/gesher-soname-fixture/
adb shell '/data/local/tmp/gesher-soname-fixture/soname_oracle /data/local/tmp/gesher-soname-fixture'
```

The fixture passed 25 checks on Android 16 ARM64. Windows component tests can
use the same oracle with the normal Gesher ELF entry points and build the
shared library for each guest ABI. Such a component run covers the shared
process setting and loader, while a packaged ART run also covers the VM's
target-SDK propagation.
