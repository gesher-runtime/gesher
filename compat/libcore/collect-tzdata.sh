#!/usr/bin/env bash
# Time zone data for $ANDROID_TZDATA_ROOT, laid out as the com.android.tzdata
# module installs it (system/timezone/apex and output_data/*/Android.bp), from
# the pinned system/timezone (docs/posix.md):
#
#   compat/libcore/collect-tzdata.sh <aosp dir> <dest dir>
#
#   <dest>/tzdata/etc/tz/{tzdata,tz_version}
#   <dest>/tzdata/etc/tz/versioned/9/{tzdata,tz_version,tzlookup.xml,telephonylookup.xml}
#   <dest>/tzdata/etc/tz/versioned/9/icu/{zoneinfo64,metaZones,timezoneTypes,windowsZones}.res
#   <dest>/TZDATA-NOTICES.txt   (IANA's notice and the ICU data's license)
set -euo pipefail

src=$1/system/timezone/output_data
dest=$2
major=9  # TzDataSetVersion.CURRENT_FORMAT_MAJOR_VERSION, AndroidDataFiles.CURRENT_MAJOR_VERSION
tz=$dest/tzdata/etc/tz
versioned=$tz/versioned/$major

mkdir -p "$versioned/icu"
cp "$src/iana/tzdata" "$src/version/tz_version" "$tz/"
cp "$src/iana/tzdata" "$src/version/tz_version" "$src/android/tzlookup.xml" \
  "$src/android/telephonylookup.xml" "$versioned/"
cp "$src/icu_overlay/zoneinfo64.res" "$src/icu_overlay/metaZones.res" \
  "$src/icu_overlay/timezoneTypes.res" "$src/icu_overlay/windowsZones.res" "$versioned/icu/"
grep -q "^0*$major\." "$src/version/tz_version" || {
  echo "tz_version $(cat "$src/version/tz_version") is not format $major" >&2
  exit 1
}
{
  echo "Time zone data (system/timezone, $(cut -d'|' -f2 "$src/version/tz_version"))"
  echo
  echo "== IANA tz database (iana/NOTICE) =="
  cat "$src/iana/NOTICE"
  echo
  echo "== ICU time zone resources (icu_overlay/LICENSE) =="
  cat "$src/icu_overlay/LICENSE"
} > "$dest/TZDATA-NOTICES.txt"
find "$dest/tzdata" -type f | sort
