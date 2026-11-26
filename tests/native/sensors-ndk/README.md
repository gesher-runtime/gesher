# Native and Java sensor contract

The probe calls the NDK sensor APIs linked from `libandroid.so`. It checks
identifier and callback event queues, timestamp domain, sensor handles,
sampling rate changes, disable and destroy. On Gesher it also compares the
native sensor inventory with Java's `SystemSensorManager`. Fixtures use a
separate process. They never change the Windows sensor configuration.

Build from a JDK and Android SDK 36 with NDK r28.

```bash
source_dir="$PWD/tests/native/sensors-ndk"
fixture_out=/absolute/path/to/sensor-fixture
sdk_jar="$ANDROID_HOME/platforms/android-36/android.jar"
ndk_bin="$ANDROID_HOME/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin"
mkdir -p "$fixture_out/classes" "$fixture_out/dex"
javac --release 8 -cp "$sdk_jar" -d "$fixture_out/classes" "$source_dir/SensorContract.java"
jar cf "$fixture_out/probe.jar" -C "$fixture_out/classes" .
"$ANDROID_HOME/build-tools/36.0.0/d8" --lib "$sdk_jar" --min-api 35 \
  --output "$fixture_out/dex" "$fixture_out/probe.jar"
for abi in x86_64 aarch64; do
  "$ndk_bin/$abi-linux-android35-clang" -Wall -Wextra -Werror -shared -fPIC \
    "$source_dir/sensor_probe.c" -landroid -o "$fixture_out/libsensor-probe-$abi.so"
done
```

Run `gesher.SensorContract <absolute guest library path> auto` through Gesher
ART with the normal distribution boot class path. Run once for each guest
ABI. It accepts an empty list on a system with no sensors, or reads the
actual selected accelerometer when available. For the deterministic case,
run a separate process with `GESHER_SENSOR_TEST=1` and pass `fixture` in
place of `auto`. That mode requires the actual configured test source and
verifies its gravity samples. The preceding Gesher implementation reports
no NDK accelerometer even with this fixture enabled.

On an Android 16 reference with an accelerometer, stage the DEX and matching
SO in an owned directory and use `app_process` with the path to the DEX in
`CLASSPATH` and the mode `android`. This mode exercises the Android NDK
reference without initializing a Java sensor manager in the shell process.
The expected final line begins with `SensorContract all passed`.
