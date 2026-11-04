# Building framework.jar: the map

What `framework-minus-apex` (frameworks/base/Android.bp) needs, as surveyed
with `tools/gen/bp_deps.py` over the `.bp` files of ~60 AOSP projects at
`android-16.0.0_r4`. Approach: [0012](decisions/0012-headless-framework.md).

```
python3 tools/gen/bp_deps.py framework-minus-apex <frameworks/base> <other checkouts>...
```

## Size
553 Soong modules are reached: 151 `aidl_interface`, 121
`java_aconfig_library` (117 `aconfig_declarations`), 71 `filegroup`, 41 Java
libraries, 17 generator modules of other kinds. frameworks/base's `core/java`
and `graphics/java` alone are 4,513 Java files and 1,532 AIDL files.

## Generated code, and the tool for each

| Code | Modules | Tool |
|---|---|---|
| R, Manifest, framework-res.apk | `framework-res` | aapt2 (build-tools) — done |
| aconfig flags | 121 libraries | `tools/gen/aconfig_java.py`, values from build/release — generator done |
| AIDL interfaces | 151 interfaces + the framework's own `.aidl` | aidl (build-tools) |
| HIDL interfaces | `android.hidl.*`, `android.hardware.*@1.x` (framework-internal-utils) | hidl-gen (system/tools/hidl) |
| Protos, lite | `framework-protos`, `ota_metadata_proto_java`, `updatable-driver-protos` | protoc |
| Protos, nano | `devicepolicyprotosnano`, `libaconfig_java_proto_nano` | protoc + protoc-gen-javanano (external/protobuf) |
| Protos, stream | `framework-javastream-protos`, `perfetto_trace_javastream_protos` | protoc + protoc-gen-javastream (frameworks/base/tools/streaming_proto) |
| Statslog | `statslog-{framework,hwui,telephony,telephony-common,expresslog}-java-gen` | stats-log-api-gen (frameworks/proto_logging) |
| Sysprop | `PlatformProperties`, `com.android.sysprop.{view,localization,foldlockbehavior,nativebinderstats}` | sysprop_java (system/tools/sysprop) |
| XSD | `apex-info-list`, `audio_policy_configuration_V7_0` | xsdc (system/tools/xsdc, Java) |
| System features | `systemfeatures-gen-srcs` | frameworks/base/tools/systemfeatures (Java) |
| Event log tags | `core/java/**/*.logtags` | build/make's java-event-log-tags.py |
| Small genrules | mime maps, `messagequeue-gen`, mock initializers, expresslog catalog | each reproduced in `tools/gen` |

The C++ tools (hidl-gen, protoc-gen-javanano, protoc-gen-javastream,
stats-log-api-gen, sysprop_java) are built for the Linux host in CI from
their sources; protoc from the release `external/protobuf` pins.

## Libraries compiled in from source
modules-utils (binary-xml, build, os, preconditions, statemachine, …),
aconfig's Java storage readers (build/make/tools/aconfig), bouncycastle
(repackaged), libphonenumber, rappor, protobuf-nano's runtime, perfetto's
framework Java library, mimemap.

## Compile-only
Annotations (`androidx.annotation`, `app-compat-annotations`,
`unsupportedappusage`, `framework-annotations-lib`, error-prone's, jsr305)
and the API stubs of the mainline modules the framework calls
(`framework-updatable-stubs-module_libs_api`, `framework-statsd`), and
`ext` (tagsoup), a boot jar of its own on Android: the
framework compiles against them; at run time a module's own jar is needed
only when the headless lifecycle reaches it. tagsoup's classes (`ext`) are
needed by `android.text.Html.fromHtml`: `app/build.sh` copies them from the
framework build's classes into gesher-app.jar.

## gesher-app.jar without building the framework
`app/build.sh` needs framework.jar's classes and libcore's java.base, not the
framework build: the `framework-classes` artifact of a CI run (the `ci`
workflow's framework job; `framework-classes.zip` and `coremod.zip`, unzipped)
has both. CompatChangesGen reads `@ChangeId` fields from frameworks/base's Java
sources, which `tools/fetch-aosp.sh`'s sparse set leaves out (the framework
build's `--checkout` widens it). Without them it finds no changes and every
app-compat change is enabled for every app. On Windows (Git Bash, a JDK 21 on
PATH):

```
tools/fetch-aosp.sh D:/gesher-dev/aosp
git -C D:/gesher-dev/aosp/frameworks/base sparse-checkout add core/java/ graphics/java/ \
  media/java/ location/java/ telephony/java/ keystore/java/
gh run download <run> -R gesher-runtime/gesher -n framework-classes -D fc   # then unzip coremod.zip into core/
AOSP_DIR=D:/gesher-dev/aosp R8_JAR=<r8.jar> app/build.sh out/app \
  --framework-classes fc/framework-classes.zip --core-classes core
```

CompatChangesGen then reports its changes (164 at android-16.0.0_r4). Take the
artifact from a run whose framework sources match the checkout. gesher-app.jar
is in the boot image (`framework/x86_64/boot-gesher-app.*`), so a jar built
this way does not match an installed image's.
