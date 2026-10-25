// gesher-base: the IANA id of Windows' time zone, for /etc/timezone
// (procfs.cc), which libcore reads for TimeZone.getDefault() (ADR 0045).
//
// Windows names its zones by its own ids ("Israel Standard Time"); CLDR's
// windowsZones maps each, per region, to an IANA id, as ICU's own Windows
// detection (wintz.cpp) does. The table is generated from the pinned ICU's
// windowsZones.txt (gen_windows_zones.py, posix.cmake).
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <string>

#include "internal.h"

namespace {

struct WindowsZone {
  const char* windows_id;
  const char* region;  // ISO 3166 alpha-2, or "001": the zone's default
  const char* iana;
};

const WindowsZone kZones[] = {
#include "windows_zones_table.inc"
};

const char* Find(const std::string& windows_id, const char* region) {
  for (const WindowsZone& z : kZones) {
    if (windows_id == z.windows_id && strcmp(region, z.region) == 0) return z.iana;
  }
  return nullptr;
}

// The user's region (GetUserDefaultGeoName on Windows 10 1709 and later),
// "001" if there is none.
std::string Region() {
  using GetUserDefaultGeoNameFn = int(WINAPI*)(LPWSTR, int);
  static auto get_name = reinterpret_cast<GetUserDefaultGeoNameFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetUserDefaultGeoName")));
  wchar_t name[16] = {};
  if (get_name != nullptr && get_name(name, 16) > 0 && wcslen(name) == 2) {
    return gesher::Narrow(name);
  }
  GEOID geo = GetUserGeoID(GEOCLASS_NATION);
  if (geo != GEOID_NOT_AVAILABLE && GetGeoInfoW(geo, GEO_ISO2, name, 16, 0) > 0) {
    return gesher::Narrow(name);
  }
  return "001";
}

// A zone at a fixed offset, as Java names one ("GMT+02:00").
std::string FixedOffset(LONG bias_minutes) {
  long offset = -bias_minutes;
  char id[16];
  snprintf(id, sizeof(id), "GMT%c%02ld:%02ld", offset < 0 ? '-' : '+', labs(offset) / 60,
           labs(offset) % 60);
  return offset == 0 ? "Etc/UTC" : id;
}

}  // namespace

namespace gesher {

std::string WindowsZoneId() {
  DYNAMIC_TIME_ZONE_INFORMATION tz;
  if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return "Etc/UTC";
  // "Adjust for daylight saving time automatically" turned off: Windows keeps
  // standard time all year, which no IANA zone of a DST region does.
  if (tz.DynamicDaylightTimeDisabled && tz.StandardDate.wMonth != 0) {
    return FixedOffset(tz.Bias + tz.StandardBias);
  }
  std::string key = Narrow(tz.TimeZoneKeyName);
  if (key.empty()) return FixedOffset(tz.Bias);
  std::string region = Region();
  const char* iana = Find(key, region.c_str());
  if (iana == nullptr) iana = Find(key, "001");
  return iana != nullptr ? iana : FixedOffset(tz.Bias);
}

}  // namespace gesher
