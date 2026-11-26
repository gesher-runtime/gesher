# Vulkan core discovery and queue contract

`core_discovery.c` exercises ordinary linked imports from `libvulkan.so`,
`dlopen`, `dlsym`, instance and device proc addresses, real physical-device
metadata, and a queue fill that is checked through mapped memory. The no-loader
and no-device outcomes have separate result lines. Record those outcomes as
unavailable for queue execution.

Build with JDK 17+, a local R8 JAR, and the Android NDK toolchain used for the
rest of the native fixtures.

```sh
./build.sh "$NDK_TOOLCHAIN_BIN" "$R8_JAR" out
adb push out/vulkan-core-aarch64 /data/local/tmp/vulkan-core
adb shell chmod 700 /data/local/tmp/vulkan-core
adb shell /data/local/tmp/vulkan-core
adb shell rm -f /data/local/tmp/vulkan-core
```

For Windows ART, use the built runtime with its ordinary boot classpath,
`-cp` pointing to `vulkan-core.dex`, entry class `gesher.VulkanCoreTest`, and
the absolute guest path to the matching `libvulkan-core-*.so` as its argument.
The Java entry point additionally verifies the Vulkan 1.0 bridge's layer and
extension boundary. Use both ELF ABIs. A successful queue receipt includes
`Vulkan queue wrote exact pattern to mapped buffer`.

The native reference uses the Vulkan 1.0 behavior described by the Khronos
[instance creation](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateInstance.html),
[instance dispatch](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
and [device dispatch](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceProcAddr.html)
contracts. The Android ARM64 emulator on Android 16
`BE2A.250530.026.D1/13818094` passed 103 checks and verified all 64 words
through SwiftShader. This exercises real Vulkan commands on a software ICD.
