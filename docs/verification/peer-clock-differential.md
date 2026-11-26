# pthread peer CPU clock differential on Android and Gesher

This [T02/N28](https://github.com/gesher-runtime/gesher/issues/56) probe
compiles one native Android PIE and runs the same bytes on an API 36 Android
device or emulator and through the Gesher Windows ELF launcher. Use the
matching device ABI and run x86-64 and arm64 as separate rows. Keep the JSON
report with the Windows artifact's source revision and download or build
record. The report records the Windows build and launcher hashes, the Android
build fingerprint and ABI list, the NDK and compiler versions, and the fixture
source and ELF hashes.

From a Gesher checkout on Windows with Python 3.11+, Android platform-tools,
an Android NDK supporting native API 35 and a compatible Gesher build:

```powershell
adb devices
adb -s "<serial>" shell getprop ro.build.fingerprint
python tools/compare_peer_clock.py --serial "<serial>" --fingerprint "<fingerprint>" --abi x86_64 --ndk "C:\Android\ndk\<version>" --gesher-exec "C:\Gesher\gesher-exec.exe" --report peer-clock-x86_64.json
```

Use `--abi arm64-v8a` and a new report path for arm64. Keep the matching
`gesher_arm64.dll` alongside `gesher-exec.exe` for that run. The reference
must expose the chosen ABI in its own reported supported ABI list. The tool
checks the ELF machine and the uploaded fixture's SHA-256 before running it.

The probe observes positive resolution and stable acquisitions for a live
peer, independent CPU accrual while the caller waits, errors from both
retained clock-ID operations after join, and the number of distinct peer
clocks acquired with 257 worker threads live at once. It uses relative CPU
behavior because Windows and Android account for time at different
granularity. It avoids queries using expired `pthread_t` values, which
[Bionic can reject by terminating the process](https://android.googlesource.com/platform/bionic/+/96867b148f5cc85951bc0170c4d94f8f124f9f06/docs/status.md).

The command exits 0 for a matching observation set, 1 for a measured
difference, and 2 for an inconclusive collection. The JSON retains both
process transcripts when both launches complete. The existing Windows
component suite documents a 256-ID bound, so the 257th lookup can expose a
capacity difference. These observations cover shell-native Linux/Bionic clock
contracts reached by the Gesher launcher. APK target-SDK policy, ART/JNI,
exhaustive thread lifecycle, CTS and native Windows arm64 are separate
validation paths.
