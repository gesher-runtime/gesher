#!/usr/bin/env bash
# Build lifecycle.apk, gesher-app's test app (tests/apps/lifecycle), or with APP_DIR another
# test app (<dir name>.apk).
#
#   tests/apps/lifecycle/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
#
# aapt2 compiles the resources and links them with the manifest against
# framework-res.apk, which also writes R.java; javac compiles the app against
# framework.jar's classes (and libcore's java.base, as app/build.sh does); R8 shrinks
# apps with proguard-rules.pro (GESHER_R8=0 opts out), otherwise D8 dexes them;
# the dex is added to the APK and the APK is zip-aligned. SIGN_TEST_APK=1 also signs
# the fixture with a disposable real certificate for verified-identity contracts.
# Other callers retain the existing unsigned fixture output.
#
# aapt2 and zipalign come from Google's build-tools r36.1 (BUILD_TOOLS_DIR to use an
# unpacked copy), d8 from Google's Maven repository (R8_JAR to use a local copy).
# Runs on Linux and in Git Bash on Windows.
set -euo pipefail

# Retry interrupted transfers, including curl's HTTP/2 and partial-file errors.
# Publish only a complete transfer, preserving an existing local tool on failure.
download_build_tool() (
  destination=$1
  temporary=$(mktemp "${destination}.part.XXXXXX")
  trap 'rm -f "$temporary"' EXIT
  curl -fsSL --connect-timeout 10 --max-time 120 \
    --retry 2 --retry-all-errors --retry-delay 1 --retry-max-time 360 \
    -o "$temporary" "$2"
  mv "$temporary" "$destination"
)

BUILD_TOOLS=36.1
R8_VERSION=9.5.26
# APP_DIR: another app directory to build with this script (tests/apps/components).
app=${APP_DIR:-$(cd "$(dirname "$0")" && pwd)}
name=$(basename "$app")
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
framework_res= framework= core=
while [[ $# -gt 0 ]]; do
  case $1 in
    --framework-res) framework_res=$2; shift 2 ;;
    --framework-classes) framework=$2; shift 2 ;;
    --core-classes) core=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $framework_res || -z $framework ]]; then
  echo "usage: build.sh <out-dir> --framework-res <apk> --framework-classes <dir|jar>" \
    "[--core-classes <dir>]" >&2
  exit 2
fi

# Native tools on Windows take Windows paths.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
  host=windows exe=.exe
else
  native() { echo "$1"; }
  native_list() { :; }
  host=linux exe=
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tools=${BUILD_TOOLS_DIR:-}
if [[ -z $tools ]]; then
  tools=$work/build-tools
  download_build_tool "$work/build-tools.zip" \
    "https://dl.google.com/android/repository/build-tools_r${BUILD_TOOLS}_$host.zip"
  mkdir -p "$work/build-tools.tmp"
  (cd "$work/build-tools.tmp" && jar -xf "$(native "$work/build-tools.zip")")
  mv "$work/build-tools.tmp"/* "$tools"
  chmod +x "$tools"/aapt2* "$tools"/zipalign* 2>/dev/null || true
fi
aapt2=$tools/aapt2$exe
zipalign=$tools/zipalign$exe

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  download_build_tool "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

# Resources and manifest. aapt2 wants res/<type>/ paths, relative on Windows.
(cd "$app" && "$aapt2" compile --dir res -o "$(native "$work/res.zip")")
mkdir -p "$work/gen"
"$aapt2" link -o "$(native "$work/unaligned.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$app/AndroidManifest.xml")" --java "$(native "$work/gen")" \
  --proguard "$(native "$work/aapt-rules.pro")" \
  --min-sdk-version 36 --target-sdk-version 36 "$(native "$work/res.zip")"

# Code.
system=()
if [[ -n $core ]]; then
  system=(--system=none --module-path "$(native "$core")")
fi
find "$app/java" "$work/gen" -name '*.java' > "$work/sources.txt"
native_list "$work/sources.txt"
mkdir -p "$work/classes" "$work/dex"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
  -cp "$(native "$framework")" -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"
(cd "$work" && find classes -name '*.class' > dex.txt)
if [[ ${GESHER_R8:-1} == 1 && -f "$app/proguard-rules.pro" ]]; then
  jar --create --file "$(native "$work/r8-input.jar")" --no-manifest \
    -C "$(native "$work/classes")" .
  r8_libs=(--lib "$(native "$framework")")
  if [[ -n $core ]]; then
    r8_libs+=(--lib "$(native "$core")")
  else
    java_home_dir=$(java -XshowSettings:properties -version 2>&1 | tr -d '\r' | sed -n 's/^[[:space:]]*java\.home = //p' | head -n1)
    if [[ -n $java_home_dir ]]; then
      r8_libs+=(--lib "$(native "$java_home_dir")")
    fi
  fi
  java -cp "$(native "$r8")" com.android.tools.r8.R8 --release --min-api 36 \
    "${r8_libs[@]}" \
    --pg-conf "$(native "$work/aapt-rules.pro")" \
    --pg-conf "$(native "$app/proguard-rules.pro")" \
    --pg-map-output "$(native "$out/$name-mapping.txt")" \
    --output "$(native "$work/dex")" \
    "$(native "$work/r8-input.jar")"
else
  rm -f "$out/$name-mapping.txt"
  (cd "$work" && java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
    --output dex @dex.txt)
fi

# The dex into the APK, then 4-byte alignment.
(cd "$work/dex" && jar -u -M -f "$(native "$work/unaligned.apk")" classes.dex)
"$zipalign" -f -p 4 "$(native "$work/unaligned.apk")" "$(native "$out/$name.apk")"
if [[ ${SIGN_TEST_APK:-0} == 1 ]]; then
  keytool -genkeypair -keystore "$(native "$work/fixture.p12")" -storetype PKCS12 \
    -storepass fixture-only -alias fixture -keyalg RSA -keysize 2048 -validity 36500 \
    -dname 'CN=Gesher disposable APK fixture' -noprompt > /dev/null 2>&1
  java -jar "$(native "$tools/lib/apksigner.jar")" sign \
    --ks "$(native "$work/fixture.p12")" --ks-pass pass:fixture-only \
    --ks-key-alias fixture --out "$(native "$work/signed.apk")" "$(native "$out/$name.apk")"
  java -jar "$(native "$tools/lib/apksigner.jar")" verify --verbose --print-certs \
    "$(native "$work/signed.apk")"
  mv "$work/signed.apk" "$out/$name.apk"
fi
echo "$name.apk written to $out"
