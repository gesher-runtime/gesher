// gesher bionic shim: broken-down time and strftime.
//
// bionic's LP64 struct tm is the nine ints the CRT has, then `long
// tm_gmtoff` and `const char* tm_zone` (56 bytes); time_t is 64-bit. The
// UTC conversions use the proleptic Gregorian calendar, including dates
// before 1970. Local time uses the CRT (Windows' time zone, or $TZ as the
// CRT reads it). strftime is written out for the C
// locale with every conversion bionic's has (C99/POSIX plus %k %l %P %s %+
// and the '-', '_', '0', '^', '#' flags), since the CRT's treats an unknown
// one as an invalid parameter. strftime_l (libc++'s time_put) is the same.

#include "shim_internal.h"

#include <windows.h>
#include <errno.h>
#include <gesher/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace gesher::bionic {

namespace {

struct BionicTm {
  int32_t tm_sec;
  int32_t tm_min;
  int32_t tm_hour;
  int32_t tm_mday;
  int32_t tm_mon;
  int32_t tm_year;
  int32_t tm_wday;
  int32_t tm_yday;
  int32_t tm_isdst;
  int32_t pad;
  int64_t tm_gmtoff;
  const char* tm_zone;
};
static_assert(sizeof(BionicTm) == 56);

// tzname/timezone/daylight, bionic's data symbols, kept in step by tzset.
char g_std_name[128] = "UTC";
char g_dst_name[128] = "UTC";
char* g_tzname[2] = {g_std_name, g_dst_name};
int64_t g_timezone = 0;
int32_t g_daylight = 0;
const char kUtc[] = "UTC";

// The CRT's zone name is Windows' (GetTimeZoneInformation's), in the ANSI code page:
// on a Hebrew Windows "שעון רגיל של ישראל" in windows-1255. Apps take tzname and
// tm_zone as UTF-8 (Dart's DateTime.timeZoneName aborted on it: Otzaria crashed).
void CrtZoneName(int index, char* out, size_t size) {
  char ansi[128];
  size_t n = 0;
  wchar_t wide[128];
  if (_get_tzname(&n, ansi, sizeof(ansi), index) != 0 ||
      MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, 128) == 0 ||
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(size), nullptr,
                          nullptr) == 0) {
    std::strcpy(out, "UTC");
  }
}

void BIONIC_SYSV Tzset() {
  _tzset();
  long tz = 0;
  int dl = 0;
  _get_timezone(&tz);
  _get_daylight(&dl);
  g_timezone = tz;
  g_daylight = dl;
  CrtZoneName(0, g_std_name, sizeof(g_std_name));
  CrtZoneName(1, g_dst_name, sizeof(g_dst_name));
}

void FromCrt(const struct tm& in, BionicTm* out) {
  out->tm_sec = in.tm_sec;
  out->tm_min = in.tm_min;
  out->tm_hour = in.tm_hour;
  out->tm_mday = in.tm_mday;
  out->tm_mon = in.tm_mon;
  out->tm_year = in.tm_year;
  out->tm_wday = in.tm_wday;
  out->tm_yday = in.tm_yday;
  out->tm_isdst = in.tm_isdst;
  out->pad = 0;
}
struct tm ToCrt(const BionicTm* in) {
  struct tm t {};
  t.tm_sec = in->tm_sec;
  t.tm_min = in->tm_min;
  t.tm_hour = in->tm_hour;
  t.tm_mday = in->tm_mday;
  t.tm_mon = in->tm_mon;
  t.tm_year = in->tm_year;
  t.tm_wday = in->tm_wday;
  t.tm_yday = in->tm_yday;
  t.tm_isdst = in->tm_isdst;
  return t;
}

// Floor division is needed for the years and seconds before the epoch. All
// divisors here are positive; unlike negating the dividend this also works
// for INT64_MIN.
int64_t UtcFloorDiv(int64_t value, int64_t divisor) {
  return value / divisor - (value % divisor < 0);
}

struct UtcDate {
  int64_t year;
  int month;  // January = 0.
  int day;
};

// March-based 400-year eras have exactly 146097 days. Keeping the leap day
// at the end of the era makes both directions work for negative years too.
UtcDate UtcDateFromDays(int64_t days) {
  int64_t shifted = days + 719468;
  int64_t era = UtcFloorDiv(shifted, 146097);
  int64_t day_of_era = shifted - era * 146097;
  int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                         day_of_era / 146096) / 365;
  int64_t year = era * 400 + year_of_era;
  int day_of_year = static_cast<int>(day_of_era -
      (365 * year_of_era + year_of_era / 4 - year_of_era / 100));
  int march_month = (5 * day_of_year + 2) / 153;
  int day = day_of_year - (153 * march_month + 2) / 5 + 1;
  int month = march_month + (march_month < 10 ? 2 : -10);
  return {year + (month < 2), month, day};
}

int64_t UtcMonthStart(int64_t year, int month) {
  year -= month < 2;
  int64_t era = UtcFloorDiv(year, 400);
  int64_t year_of_era = year - era * 400;
  int march_month = month + (month < 2 ? 10 : -2);
  int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                       (153 * march_month + 2) / 5;
  return era * 146097 + day_of_era - 719468;
}

BionicTm* BIONIC_SYSV GmtimeR(const int64_t* t, BionicTm* out) {
  int64_t days = *t / 86400;
  int64_t seconds = *t % 86400;
  if (seconds < 0) {
    seconds += 86400;
    --days;
  }
  UtcDate date = UtcDateFromDays(days);
  out->tm_zone = kUtc;
  // Like bionic's checked conversion, reject only a year that cannot be
  // represented by struct tm, not the CRT's narrower 1970..3000 range.
  if (__builtin_sub_overflow(date.year, int64_t{1900}, &out->tm_year))
    return errno = 75 /* EOVERFLOW */, nullptr;
  out->tm_sec = static_cast<int32_t>(seconds % 60);
  out->tm_min = static_cast<int32_t>(seconds / 60 % 60);
  out->tm_hour = static_cast<int32_t>(seconds / 3600);
  out->tm_mday = date.day;
  out->tm_mon = date.month;
  out->tm_wday = static_cast<int32_t>((days + 4) % 7);
  if (out->tm_wday < 0) out->tm_wday += 7;
  out->tm_yday = static_cast<int32_t>(days - UtcMonthStart(date.year, 0));
  out->tm_isdst = 0;
  out->pad = 0;
  out->tm_gmtoff = 0;
  return out;
}

BionicTm* BIONIC_SYSV LocaltimeR(const int64_t* t, BionicTm* out) {
  struct tm crt;
  __time64_t v = *t;
  if (_localtime64_s(&crt, &v) != 0) return errno = 75 /* EOVERFLOW */, nullptr;
  FromCrt(crt, out);
  // The offset east of UTC: the local fields read as UTC, minus the time.
  out->tm_gmtoff = static_cast<int64_t>(_mkgmtime64(&crt)) - *t;
  out->tm_zone = crt.tm_isdst > 0 ? g_dst_name : g_std_name;
  return out;
}

thread_local BionicTm t_tm;

BionicTm* BIONIC_SYSV Gmtime(const int64_t* t) { return GmtimeR(t, &t_tm); }
BionicTm* BIONIC_SYSV Localtime(const int64_t* t) { return LocaltimeR(t, &t_tm); }

int64_t BIONIC_SYSV Mktime(BionicTm* tm) {
  struct tm crt = ToCrt(tm);
  __time64_t t = _mktime64(&crt);
  if (t == -1) return errno = 75, -1;
  int64_t result = t;
  LocaltimeR(&result, tm);
  return result;
}
bool UtcNormalizeField(int64_t* larger, int64_t* smaller, int base) {
  int64_t carry = UtcFloorDiv(*smaller, base);
  *smaller -= carry * base;
  *larger += carry;
  // Bionic checks the int-sized minute, hour and day carries separately;
  // accepting a later cancellation would change which inputs it accepts.
  return INT32_MIN <= *larger && *larger <= INT32_MAX;
}

bool UtcTimegmAttempt(const BionicTm* in, bool normalize_seconds,
                     int64_t* result, BionicTm* normalized) {
  int64_t second = in->tm_sec;
  int64_t minute = in->tm_min;
  int64_t hour = in->tm_hour;
  int64_t day = in->tm_mday;
  if (normalize_seconds && !UtcNormalizeField(&minute, &second, 60)) return false;
  if (!UtcNormalizeField(&hour, &minute, 60) ||
      !UtcNormalizeField(&day, &hour, 24)) return false;
  int64_t month_carry = UtcFloorDiv(in->tm_mon, 12);
  int month = static_cast<int>(in->tm_mon - month_carry * 12);
  // Android LP64's int_fast32_t year accumulator is 64 bits, including on
  // our Windows host where the native int_fast32_t is only 32 bits.
  int64_t year = int64_t{in->tm_year} + month_carry + 1900;
  int64_t days = UtcMonthStart(year, month) + day - 1;
  UtcDate date = UtcDateFromDays(days);
  if (date.year - 1900 < INT32_MIN || date.year - 1900 > INT32_MAX) return false;
  // tzcode first saves out-of-range seconds, then retries with them carried
  // into minutes. Before 1970 its first attempt subtracts 59 from the saved
  // int seconds; retain that overflow boundary without overflowing here.
  if ((second < 0 || second >= 60) && date.year < 1970 &&
      second < int64_t{INT32_MIN} + 59) return false;
  // With a representable tm_year, this magnitude is below 7e16 seconds,
  // even after adding any int-sized seconds: all arithmetic fits int64_t.
  *result = days * 86400 + hour * 3600 + minute * 60 + second;
  return GmtimeR(result, normalized) != nullptr;
}

int64_t BIONIC_SYSV Timegm(BionicTm* tm) {
  int saved_errno = errno;
  int64_t result;
  BionicTm normalized {};
  if (!UtcTimegmAttempt(tm, false, &result, &normalized) &&
      !UtcTimegmAttempt(tm, true, &result, &normalized))
    return errno = 75 /* EOVERFLOW */, -1;
  // Commit only a fully representable result. In particular a failed final
  // year check must not feed a wrapped year into the second attempt.
  *tm = normalized;
  errno = saved_errno;
  return result;  // -1 itself is the valid instant just before the epoch.
}

double BIONIC_SYSV Difftime(int64_t a, int64_t b) {
  // Subtract before conversion to retain adjacent large instants, but use
  // an unsigned magnitude so the distance between signed extremes is safe.
  if (a >= b) return static_cast<double>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
  return -static_cast<double>(static_cast<uint64_t>(b) - static_cast<uint64_t>(a));
}

// --- strftime ------------------------------------------------------------------

const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                             "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};

bool IsLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// ISO 8601 week-based year and week number.
void IsoWeek(const BionicTm* tm, int64_t* year, int* week) {
  int64_t y = tm->tm_year + 1900LL;
  int yday = tm->tm_yday;
  int wday = (tm->tm_wday + 6) % 7;  // Monday = 0
  int w = (yday - wday + 10) / 7;     // 1-based
  if (w < 1) {
    --y;
    int days = IsLeap(y) ? 366 : 365;
    w = (yday + days - wday + 10) / 7;
  } else {
    int days = IsLeap(y) ? 366 : 365;
    if (yday - wday + 3 >= days) {  // belongs to week 1 of the next year
      ++y;
      w = 1;
    }
  }
  *year = y;
  *week = w;
}

void AppendNumber(std::string* out, int64_t v, int width, char pad, char flag) {
  if (flag == '-') {
    width = 0;
  } else if (flag == '_') {
    pad = ' ';
  } else if (flag == '0') {
    pad = '0';
  }
  char digits[32];
  bool neg = v < 0;
  uint64_t u = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + u % 10);
    u /= 10;
  } while (u != 0);
  int len = n + (neg ? 1 : 0);
  if (pad == '0') {
    if (neg) out->push_back('-');
    for (int i = len; i < width; ++i) out->push_back('0');
  } else {
    for (int i = len; i < width; ++i) out->push_back(' ');
    if (neg) out->push_back('-');
  }
  while (n > 0) out->push_back(digits[--n]);
}

void AppendText(std::string* out, const char* s, char flag, bool upper_hash) {
  for (; *s; ++s) {
    char c = *s;
    if (flag == '^' || upper_hash) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    out->push_back(c);
  }
}

std::string Strftime(const char* fmt, const BionicTm* tm) {
  std::string out;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      out.push_back(*p);
      continue;
    }
    const char* start = p;
    ++p;
    char flag = 0;
    while (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') flag = *p++;
    int width = -1;
    while (*p >= '0' && *p <= '9') width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
    if (*p == 'E' || *p == 'O') ++p;  // locale modifiers: the C locale's
    char c = *p;
    if (c == 0) {
      out.append(start);
      break;
    }
    int hour12 = tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12;
    int64_t year = tm->tm_year + 1900LL;
    auto num = [&](int64_t v, int w, char pad) {
      AppendNumber(&out, v, width >= 0 ? width : w, pad, flag);
    };
    auto text = [&](const char* s) {
      std::string t;
      AppendText(&t, s, flag, false);
      for (int i = static_cast<int>(t.size()); i < width; ++i) out.push_back(' ');
      out += t;
    };
    auto sub = [&](const char* f) {
      std::string t = Strftime(f, tm);
      if (flag == '^') AppendText(&out, t.c_str(), '^', false); else out += t;
    };
    bool day_ok = tm->tm_wday >= 0 && tm->tm_wday < 7;
    bool mon_ok = tm->tm_mon >= 0 && tm->tm_mon < 12;
    switch (c) {
      case '%': out.push_back('%'); break;
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'a': text(day_ok ? std::string(kDays[tm->tm_wday], 3).c_str() : "?"); break;
      case 'A': text(day_ok ? kDays[tm->tm_wday] : "?"); break;
      case 'b':
      case 'h': text(mon_ok ? std::string(kMonths[tm->tm_mon], 3).c_str() : "?"); break;
      case 'B': text(mon_ok ? kMonths[tm->tm_mon] : "?"); break;
      case 'c': sub("%a %b %e %H:%M:%S %Y"); break;
      case 'C': num(year / 100, 2, '0'); break;
      case 'd': num(tm->tm_mday, 2, '0'); break;
      case 'D': sub("%m/%d/%y"); break;
      case 'e': num(tm->tm_mday, 2, ' '); break;
      case 'F': sub("%Y-%m-%d"); break;
      case 'g': case 'G': case 'V': {
        int64_t iso_year;
        int week;
        IsoWeek(tm, &iso_year, &week);
        if (c == 'g') num(iso_year % 100, 2, '0');
        else if (c == 'G') num(iso_year, 4, '0');
        else num(week, 2, '0');
        break;
      }
      case 'H': num(tm->tm_hour, 2, '0'); break;
      case 'I': num(hour12, 2, '0'); break;
      case 'j': num(tm->tm_yday + 1, 3, '0'); break;
      case 'k': num(tm->tm_hour, 2, ' '); break;
      case 'l': num(hour12, 2, ' '); break;
      case 'm': num(tm->tm_mon + 1, 2, '0'); break;
      case 'M': num(tm->tm_min, 2, '0'); break;
      case 'p': text(tm->tm_hour < 12 ? "AM" : "PM"); break;
      case 'P': text(tm->tm_hour < 12 ? "am" : "pm"); break;
      case 'r': sub("%I:%M:%S %p"); break;
      case 'R': sub("%H:%M"); break;
      case 's': {
        BionicTm copy = *tm;
        struct tm crt = ToCrt(&copy);
        num(static_cast<int64_t>(_mktime64(&crt)), 1, '0');
        break;
      }
      case 'S': num(tm->tm_sec, 2, '0'); break;
      case 'T': sub("%H:%M:%S"); break;
      case 'u': num(tm->tm_wday == 0 ? 7 : tm->tm_wday, 1, '0'); break;
      case 'U': num((tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0'); break;
      case 'w': num(tm->tm_wday, 1, '0'); break;
      case 'W': num((tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, '0'); break;
      case 'x': sub("%m/%d/%y"); break;
      case 'X': sub("%H:%M:%S"); break;
      case 'y': num(((year % 100) + 100) % 100, 2, '0'); break;
      case 'Y': num(year, 1, '0'); break;
      case 'z': {
        int64_t off = tm->tm_gmtoff;
        out.push_back(off < 0 ? '-' : '+');
        if (off < 0) off = -off;
        AppendNumber(&out, off / 3600 * 100 + off / 60 % 60, 4, '0', 0);
        break;
      }
      case 'Z': text(tm->tm_zone != nullptr ? tm->tm_zone : ""); break;
      case '+': sub("%a %b %e %H:%M:%S %Z %Y"); break;
      default:  // unknown: copy it through, as bionic does
        out.append(start, static_cast<size_t>(p - start + 1));
        break;
    }
  }
  return out;
}

size_t BIONIC_SYSV StrftimeEntry(char* s, size_t max, const char* fmt, const BionicTm* tm) {
  std::string r = Strftime(fmt, tm);
  if (r.size() + 1 > max) return 0;
  std::memcpy(s, r.c_str(), r.size() + 1);
  return r.size();
}
size_t BIONIC_SYSV StrftimeL(char* s, size_t max, const char* fmt, const BionicTm* tm, void*) {
  return StrftimeEntry(s, max, fmt, tm);
}

char* BIONIC_SYSV AsctimeR(const BionicTm* tm, char* buf) {
  std::string r = Strftime("%a %b %e %H:%M:%S %Y\n", tm);
  std::memcpy(buf, r.c_str(), r.size() + 1 < 26 ? r.size() + 1 : 26);
  buf[25] = 0;
  return buf;
}
thread_local char t_asctime[32];
char* BIONIC_SYSV Asctime(const BionicTm* tm) { return AsctimeR(tm, t_asctime); }
char* BIONIC_SYSV CtimeR(const int64_t* t, char* buf) {
  BionicTm tm;
  if (LocaltimeR(t, &tm) == nullptr) return nullptr;
  return AsctimeR(&tm, buf);
}
char* BIONIC_SYSV Ctime(const int64_t* t) { return CtimeR(t, t_asctime); }

// --- timerfd -----------------------------------------------------------------
// Host descriptor ownership is shared with dup/close, in gesher-base. The
// LP64 guest values are unchanged; only the guest calling convention lives here.
using LinuxItimerspec = gesher::TimerFdSpec;
static_assert(sizeof(gesher::TimerFdTime) == sizeof(LinuxTimespec));

int BIONIC_SYSV TimerfdCreate(int clockid, int flags) {
  return gesher::TimerFdCreate(clockid, flags);
}

int BIONIC_SYSV TimerfdGettime(int fd, LinuxItimerspec* value) {
  return gesher::TimerFdGetTime(fd, value);
}

int BIONIC_SYSV TimerfdSettime(int fd, int flags, const LinuxItimerspec* value,
                              LinuxItimerspec* previous) {
  return gesher::TimerFdSetTime(fd, flags, value, previous);
}

}  // namespace

bool DispatchTimerfdSyscall(int64_t nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                           int64_t* result) {
  // Linux x86-64 syscall table: timerfd_create=283, settime=286, gettime=287.
  // Keep the existing LP64 timespec layout, validation, errno and timer lifecycle.
  switch (nr) {
    case 283:
      *result = TimerfdCreate(static_cast<int>(a1), static_cast<int>(a2));
      return true;
    case 286:
      *result = TimerfdSettime(static_cast<int>(a1), static_cast<int>(a2),
          reinterpret_cast<const LinuxItimerspec*>(a3), reinterpret_cast<LinuxItimerspec*>(a4));
      return true;
    case 287:
      *result = TimerfdGettime(static_cast<int>(a1), reinterpret_cast<LinuxItimerspec*>(a2));
      return true;
    default:
      return false;
  }
}

void RegisterTime(SymbolTable& t) {
  Tzset();
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("tzset", Tzset);
  E("gmtime_r", GmtimeR);
  E("gmtime", Gmtime);
  E("gmtime64_r", GmtimeR);
  E("localtime_r", LocaltimeR);
  E("localtime", Localtime);
  E("localtime64_r", LocaltimeR);
  E("mktime", Mktime);
  E("mktime64", Mktime);
  E("timelocal", Mktime);
  E("timegm", Timegm);
  E("timegm64", Timegm);
  E("difftime", Difftime);
  E("strftime", StrftimeEntry);
  E("strftime_l", StrftimeL);
  E("asctime", Asctime);
  E("asctime_r", AsctimeR);
  E("ctime", Ctime);
  E("ctime_r", CtimeR);
  E("timerfd_create", TimerfdCreate);
  E("timerfd_settime", TimerfdSettime);
  E("timerfd_settime64", TimerfdSettime);
  E("timerfd_gettime", TimerfdGettime);
  E("timerfd_gettime64", TimerfdGettime);
#undef E
  t["tzname"] = g_tzname;
  t["timezone"] = &g_timezone;
  t["daylight"] = &g_daylight;
}

}  // namespace gesher::bionic
