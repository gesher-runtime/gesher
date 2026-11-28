# SDK 36 boot class resolution

This probe calls `Class.forName(name, false, null)` once for every class
definition in the SDK 36 platform JAR. It records bootstrap visibility and
linkage without initializing the requested classes. Android and Windows run
the same DEX and class list. The comparison rejects missing or duplicate
records. Installed APK, split and named shared-library resolution require
their own class-loader and lifecycle cases.

Build with Android SDK Platform 36, Build Tools 36.0.0, Python and a JDK.

```sh
./build.sh "$ANDROID_SDK_ROOT" out
adb push out/boot-census.dex /data/local/tmp/gesher-boot-census.dex
adb push out/sdk36-classes.txt /data/local/tmp/gesher-boot-census.txt
adb shell 'CLASSPATH=/data/local/tmp/gesher-boot-census.dex app_process /system/bin BootClassCensus /data/local/tmp/gesher-boot-census.txt' >out/android.log
```

Run `BootClassCensus` on Gesher using `dalvikvm.exe -cp`, the DEX and
class-list paths, and the same five boot JARs and boot image that the
managed launcher uses. Keep the launcher-installed resources and data
directories in an isolated test profile. Save stdout as `out/windows.log`.

```sh
python3 compare.py out/sdk36-classes.txt out/android.log out/windows.log
```

## Recorded result

The class list from Platform 36 r02 had 6,227 entries. Android 16 ARM64
(`BE2A.250530.026.D1/13818094`) resolved 6,225. Gesher build 0.2.0.3123
(`156abd94`, run 45668397258) resolved 5,429 on Windows 11 26H2 ARM64
using the x64 emulator. All 5,429 resolved on both platforms. Android alone
resolved 796. Both lacked the SDK-only `androidx.annotation.RecentlyNonNull`
and `RecentlyNullable` entries. Neither encountered a linkage or security
exception in this probe.

The DEX SHA-256 was `dcd65d4053f4eca3f4da7e0bcc338ad7689229cd38c9b9e9af964baf49140e03`.
The input-list SHA-256 was `14d292f34db93d49814bac65948b6b14e2d4f36d699458101bf66c2d44a2c36c`.
The static A02 audit on the same five JARs agreed on all 798 absences. Its
member inventory counted 90,630 SDK records, with 83,907 exact descriptors,
503 potential access flag differences, 170 missing descriptors on present
classes and 6,050 entries on absent classes.

Visibility counts do not establish behavior. The Windows execution here
belongs to the retained build. The issue's release-wide loader classification
and app-level coverage remain open. [Roadmap A02/T02](https://github.com/gesher-runtime/gesher/issues/56)
