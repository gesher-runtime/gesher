#!/usr/bin/env bash
# Build gesher-app.jar: the app driver (app/java), dexed for the boot class path.
#
#   app/build.sh <out-dir> --framework-classes <dir|jar> [--core-classes <dir>]
#
# --framework-classes: framework.jar's classes, to compile against (the javac output
#   of tools/framework/build.py, or a jar of them).
# --core-classes: libcore's java.base as an exploded module (module-info.class at its
#   root), as tools/framework/build.py takes it. Without it the JDK's java.base stands
#   in, which is enough to compile but not a check against Android's libcore.
#
# Writes <out-dir>/gesher-app.jar (classes.dex), gesher-app-classes.jar (compile-only),
# and <out-dir>/build.prop. gesher-app.jar
# goes on -Xbootclasspath after framework.jar (see app/README.md). Needs a JDK 17+ and
# curl; d8 comes from Google's Maven repository (R8_JAR to use a local copy). Runs on
# Linux and in Git Bash on Windows.
set -euo pipefail

R8_VERSION=9.5.26
APKSIG_VERSION=8.13.0
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
framework= core=
while [[ $# -gt 0 ]]; do
  case $1 in
    --framework-classes) framework=$2; shift 2 ;;
    --core-classes) core=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $framework ]]; then
  echo "usage: app/build.sh <out-dir> --framework-classes <dir|jar> [--core-classes <dir>]" >&2
  exit 2
fi
framework=$(cd "$(dirname "$framework")" && pwd)/$(basename "$framework")
if [[ -n $core ]]; then core=$(cd "$core" && pwd); fi

# Java on Windows takes Windows paths and ';'-separated lists.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
else
  native() { echo "$1"; }
  native_list() { :; }
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi
apksig=${APKSIG_JAR:-$work/apksig.jar}
if [[ ! -f $apksig ]]; then
  curl -fsSL -o "$apksig" \
    "https://dl.google.com/android/maven2/com/android/tools/build/apksig/$APKSIG_VERSION/apksig-$APKSIG_VERSION.jar"
fi
apksig=$(cd "$(dirname "$apksig")" && pwd)/$(basename "$apksig")
driverCp=$(native "$framework")
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then driverCp+=";"; else driverCp+=":"; fi
driverCp+=$(native "$apksig")

# No-op initializers and placeholders for classes framework.jar compiles against but
# does not contain (see app/README.md). They depend on nothing.
mkdir -p "$work/compat"
find "$root/app/compat-java" -name '*.java' > "$work/compat.txt"
native_list "$work/compat.txt"
javac --release 11 -nowarn -d "$(native "$work/compat")" @"$(native "$work/compat.txt")"

# The driver, against framework.jar's classes and libcore's java.base, as framework.jar
# itself is compiled (tools/framework/build.py).
system=()
if [[ -n $core ]]; then
  system=(--system=none --module-path "$(native "$core")")
  while IFS= read -r pkg; do
    system+=(--add-exports "java.base/$pkg=ALL-UNNAMED")
  done < <(cd "$core" && find . -name '*.class' ! -name module-info.class -printf '%h\n' \
             | sed 's|^\./||; s|/|.|g' | sort -u)
fi
# Concrete IFoo.Stub classes for the system services gesher serves over binder
# (com.gesher.server.StandIn), generated from framework.jar's interfaces.
gencp=$(native "$framework")
if [[ -n $core ]]; then
  if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then gencp+=";"; else gencp+=":"; fi
  gencp+=$(native "$core")
fi
java -XX:+UnlockDiagnosticVMOptions -XX:-BytecodeVerificationRemote -cp "$gencp" "$(native "$root/app/gen/StubGen.java")" \
  "$(native "$work/gen")" com.gesher.server.stub \
  android.permission.IPermissionChecker \
  android.app.IActivityManager android.app.IActivityTaskManager android.app.IUriGrantsManager \
  android.app.IActivityClientController android.content.pm.IPackageManager android.content.pm.IPackageInstaller android.content.pm.IPackageInstallerSession \
  android.content.IRestrictionsManager android.app.blob.IBlobStoreManager android.os.IDeviceIdentifiersPolicyService \
  android.content.rollback.IRollbackManager \
  android.view.IWindowManager android.view.IWindowSession \
  android.hardware.display.IDisplayManager android.os.IUserManager   com.android.internal.view.IInputMethodManager \
  android.app.INotificationManager android.app.IUiModeManager android.hardware.input.IInputManager \
  android.media.IAudioService android.content.IContentService android.os.storage.IStorageManager \
  android.media.session.ISessionManager android.media.session.ISession android.media.session.ISessionController \
  android.os.IPowerManager android.os.IDeviceIdleController android.os.IThermalService android.app.IAlarmManager android.content.IClipboard \
  com.android.internal.app.IAppOpsService com.android.internal.appwidget.IAppWidgetService \
  android.app.ISearchManager android.app.ILocaleManager android.app.IGrammaticalInflectionManager android.app.job.IJobScheduler android.os.IBatteryPropertiesRegistrar com.android.internal.app.IBatteryStats android.location.ILocationManager android.os.IVibratorManagerService \
  android.accounts.IAccountManager \
  com.android.internal.os.IDropBoxManagerService \
  android.app.usage.IUsageStatsManager \
  android.app.admin.IDevicePolicyManager \
  com.android.internal.telephony.ITelephony \
  com.android.internal.telephony.IPhoneSubInfo \
  com.android.internal.telephony.ISub \
  com.android.internal.telephony.ITelephonyRegistry \
  android.app.trust.ITrustManager \
  android.content.pm.IShortcutService \
  android.content.pm.ICrossProfileApps \
  android.view.ISensitiveContentProtectionManager \
  android.permission.IPermissionManager
# The framework's app-compat changes (@ChangeId fields), for bindApplication's
# disabledCompatChanges (com.gesher.server.CompatChanges, docs/e2e.md). The annotations
# have SOURCE retention: they are read from frameworks/base's sources (what the
# framework build checked out, $AOSP_DIR or third_party/aosp).
aosp=${AOSP_DIR:-$root/third_party/aosp}
java -cp "$gencp" "$(native "$root/app/gen/CompatChangesGen.java")" "$(native "$framework")" \
  "$(native "$work/gen")" com.gesher.server.gen "$(native "$aosp/frameworks/base")"

mkdir -p "$work/classes"
# Not app/java/com/google: those are client-SDK stubs (play-services, Firebase) that
# apps bundle in their own dex (tests/apps/gmsapp/build.sh compiles them into
# gmsapp.apk). On the boot class path they would shadow an APK's real copies
# (parent-first loading): a stub FirebaseApp.get() answering null is
# "FirebaseCrashlytics component is not present".
find "$root/app/java" "$work/gen" -name '*.java' -not -path "$root/app/java/com/google/*" \
  > "$work/sources.txt"
native_list "$work/sources.txt"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
  -cp "$driverCp" -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"

# framework-location (frameworks/base/location/java: LocationManager, ILocationManager...):
# on a device its own jar on the boot class path, which framework.jar is compiled against
# but does not contain. The framework build compiles it (compile-only sources); its
# classes join gesher-app.jar, already on Gesher's boot class path (docs/devices.md).
mkdir -p "$work/location"
loc=$aosp/frameworks/base/location/java
fwloc=$framework
if [[ -f $framework && -d $loc ]]; then
  # A jar of the framework's classes: the location packages out of it.
  fwloc=$work/fwloc
  mkdir -p "$fwloc"
  (cd "$fwloc" && unzip -q -o "$framework" 'android/location/*' 'android/location/*/*' 'com/android/internal/location/*' 'com/android/internal/location/*/*' || true)
fi
if [[ -d $fwloc && -d $loc ]]; then
  while IFS= read -r c; do
    for f in "$fwloc/$c.class" "$fwloc/$c"\$*.class; do
      if [[ -f $f ]]; then
        mkdir -p "$work/location/$(dirname "$c")"
        cp "$f" "$work/location/$(dirname "$c")/"
        # The real class replaces app/compat-java's placeholder (LocationManager, CountryDetector).
        rm -f "$work/compat/$(dirname "$c")/$(basename "$f")"
      fi
    done
  done < <(cd "$loc" && find . -name '*.java' -o -name 'I*.aidl' | sed 's|^\./||; s|\.java$||; s|\.aidl$||')
  echo "framework-location: $(find "$work/location" -name '*.class' | wc -l) classes into gesher-app.jar"
fi

# ext (external/tagsoup, org.ccil.cowan.tagsoup): on a device a boot jar of its own
# (frameworks/base's "ext"), which framework.jar is compiled against but does not contain.
# android.text.Html.fromHtml parses with its Parser and HTMLSchema: without them every
# fromHtml call died on a stackless NoClassDefFoundError ("Class not found using the boot
# class loader"), which Compose apps hit while composing (Wikipedia's onboarding). The
# framework build compiles tagsoup's sources (compile only); its classes join gesher-app.jar.
mkdir -p "$work/ext"
if [[ -f $framework ]]; then
  (cd "$work/ext" && unzip -q -o "$framework" 'org/ccil/cowan/tagsoup/*' || true)
elif [[ -d $framework/org/ccil/cowan/tagsoup ]]; then
  mkdir -p "$work/ext/org/ccil/cowan"
  cp -R "$framework/org/ccil/cowan/tagsoup" "$work/ext/org/ccil/cowan/"
fi
tagsoup=$(find "$work/ext" -name '*.class' | wc -l)
if [[ $tagsoup -eq 0 ]]; then
  echo "warning: no org.ccil.cowan.tagsoup classes in $framework: android.text.Html.fromHtml will fail" >&2
fi
echo "ext (tagsoup): $tagsoup classes into gesher-app.jar"

# Official Apache-2.0 apksig implementation; exclude any apksigner CLI in a supplied
# SDK apksigner.jar. The runtime uses only the APK verifier library.
mkdir -p "$work/apksig"
(cd "$work/apksig" && unzip -q -o "$apksig")

# Compile-only API overlay for tests of classes supplied by gesher-app.
# Distribution packaging names the runtime DEX archives explicitly.
jar --create --file "$(native "$out/gesher-app-classes.jar")" --no-manifest \
  -C "$(native "$work/classes")" . -C "$(native "$work/compat")" . \
  -C "$(native "$work/location")" . -C "$(native "$work/ext")" .

# One dex for all, into a jar.
( cd "$work" && find classes compat location ext apksig/com/android/apksig -name '*.class' ) > "$work/dex.txt"
(cd "$work" && java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$(native "$out/gesher-app.jar")" @dex.txt)
cp "$root/app/build.prop" "$out/build.prop"
echo "gesher-app written to $out"

# The genuine AOSP shared library required by the unmodified GmsCore APK. It is
# loaded through <uses-library>, never appended to the boot class path.
provider=$aosp/frameworks/base/location/lib/java
if [[ -d $provider ]]; then
  mkdir -p "$work/provider" "$out/etc/permissions"
  providerAnnotations=${ANDROIDX_ANNOTATION_JAR:-$work/annotation-jvm.jar}
  if [[ ! -f $providerAnnotations ]]; then
    curl -fsSL -o "$providerAnnotations" \
      https://dl.google.com/android/maven2/androidx/annotation/annotation-jvm/1.9.1/annotation-jvm-1.9.1.jar
  fi
  providerCp=$(native "$framework")
  if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then providerCp+=";"; else providerCp+=":"; fi
  providerCp+=$(native "$providerAnnotations")
  find "$provider" -name '*.java' > "$work/provider.txt"
  native_list "$work/provider.txt"
  javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
    -cp "$providerCp" -d "$(native "$work/provider")" @"$(native "$work/provider.txt")"
  (cd "$work/provider" && find . -name '*.class') > "$work/provider-dex.txt"
  (cd "$work/provider" && java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
    --output "$(native "$out/com.android.location.provider.jar")" @"$(native "$work/provider-dex.txt")")
  cp "$root/tools/package/etc/permissions/com.android.location.provider.xml" "$out/etc/permissions/"
  echo "com.android.location.provider written to $out"
fi
