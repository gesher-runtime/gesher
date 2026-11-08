// gesher bionic shim: broken-down time and strftime.
//
// bionic's LP64 struct tm is the nine ints the CRT has, then `long
// tm_gmtoff` and `const char* tm_zone` (56 bytes); time_t is 64-bit. The
// conversions use the CRT's 64-bit time functions (local time is Windows'
// time zone, or $TZ as the CRT reads it). strftime is written out for the C
// locale with every conversion bionic's has (C99/POSIX plus %k %l %P %s %+
// and the '-', '_', '0', '^', '#' flags), since the CRT's treats an unknown
// one as an invalid parameter. strftime_l (libc++'s time_put) is the same.

#include "shim_internal.h"

#include <windows.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

BionicTm* BIONIC_SYSV GmtimeR(const int64_t* t, BionicTm* out) {
  struct tm crt;
  __time64_t v = *t;
  if (_gmtime64_s(&crt, &v) != 0) return errno = 75 /* EOVERFLOW */, nullptr;
  FromCrt(crt, out);
  out->tm_gmtoff = 0;
  out->tm_zone = kUtc;
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
int64_t BIONIC_SYSV Timegm(BionicTm* tm) {
  struct tm crt = ToCrt(tm);
  __time64_t t = _mkgmtime64(&crt);
  if (t == -1) return errno = 75, -1;
  int64_t result = t;
  GmtimeR(&result, tm);
  return result;
}
double BIONIC_SYSV Difftime(int64_t a, int64_t b) { return static_cast<double>(a - b); }

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

constexpr int kTfdTimerAbstime = 1 << 0;
constexpr int kTfdCloexec = 02000000;
constexpr int kTfdNonblock = 04000;

constexpr int kClockRealtime = 0;
constexpr int kClockMonotonic = 1;
constexpr int kClockMonotonicRaw = 4;
constexpr int kClockBoottime = 7;

struct LinuxItimerspec {
  LinuxTimespec it_interval;
  LinuxTimespec it_value;
};
static_assert(sizeof(LinuxItimerspec) == 32, "LinuxItimerspec size mismatch");

struct TimerState {
  int fd = -1;
  int clockid = 0;
  PTP_TIMER tp_timer = nullptr;
  std::mutex mtx;
  LinuxItimerspec spec{};
  int64_t next_expire_ns = 0;
  int64_t interval_ns = 0;

  ~TimerState() {
    if (tp_timer != nullptr) {
      SetThreadpoolTimer(tp_timer, nullptr, 0, 0);
      WaitForThreadpoolTimerCallbacks(tp_timer, TRUE);
      CloseThreadpoolTimer(tp_timer);
      tp_timer = nullptr;
    }
  }
};

std::mutex g_timer_mutex;
std::unordered_map<int, std::shared_ptr<TimerState>> g_timers;

int64_t MonotonicNowNanos() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<int64_t>((static_cast<__int128>(now.QuadPart) * 1000000000LL) / freq.QuadPart);
}

int64_t RealtimeNowNanos() {
  LinuxTimespec ts = RealtimeNow();
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

VOID CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
  auto* state = static_cast<TimerState*>(context);
  if (state == nullptr) return;

  {
    std::lock_guard<std::mutex> lock(state->mtx);
    if (state->interval_ns > 0) {
      state->next_expire_ns += state->interval_ns;
    } else {
      state->next_expire_ns = 0;
    }
  }

  uint64_t val = 1;
  int res = eventfd_write(state->fd, val);
  if (res != 0 && errno == kEBADF) {
    SetThreadpoolTimer(state->tp_timer, nullptr, 0, 0);
  }
}

int BIONIC_SYSV TimerfdCreate(int clockid, int flags) {
  if (clockid != kClockRealtime && clockid != kClockMonotonic &&
      clockid != kClockBoottime && clockid != kClockMonotonicRaw) {
    errno = kEINVAL;
    return -1;
  }
  if ((flags & ~(kTfdNonblock | kTfdCloexec)) != 0) {
    errno = kEINVAL;
    return -1;
  }

  int efd_flags = 0;
  if ((flags & kTfdNonblock) != 0) efd_flags |= EFD_NONBLOCK;
  if ((flags & kTfdCloexec) != 0) efd_flags |= EFD_CLOEXEC;

  int fd = eventfd(0, efd_flags);
  if (fd < 0) return -1;

  auto state = std::make_shared<TimerState>();
  state->fd = fd;
  state->clockid = clockid;
  state->tp_timer = CreateThreadpoolTimer(&TimerCallback, state.get(), nullptr);
  if (state->tp_timer == nullptr) {
    close(fd);
    errno = kENOMEM;
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(g_timer_mutex);
    auto it = g_timers.find(fd);
    if (it != g_timers.end()) {
      g_timers.erase(it);
    }
    g_timers[fd] = state;
  }

  return fd;
}

int BIONIC_SYSV TimerfdGettime(int fd, LinuxItimerspec* curr_value) {
  if (curr_value == nullptr) {
    errno = kEFAULT;
    return -1;
  }

  std::shared_ptr<TimerState> state;
  {
    std::lock_guard<std::mutex> lock(g_timer_mutex);
    auto it = g_timers.find(fd);
    if (it == g_timers.end()) {
      errno = kEBADF;
      return -1;
    }
    state = it->second;
  }

  std::lock_guard<std::mutex> lock(state->mtx);
  curr_value->it_interval = state->spec.it_interval;

  if (state->next_expire_ns == 0) {
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_nsec = 0;
  } else {
    int64_t now_ns = (state->clockid == kClockRealtime) ? RealtimeNowNanos() : MonotonicNowNanos();
    int64_t rem_ns = state->next_expire_ns - now_ns;
    if (rem_ns <= 0) {
      if (state->interval_ns > 0) {
        curr_value->it_value.tv_sec = 0;
        curr_value->it_value.tv_nsec = 1;
      } else {
        curr_value->it_value.tv_sec = 0;
        curr_value->it_value.tv_nsec = 0;
      }
    } else {
      curr_value->it_value.tv_sec = rem_ns / 1000000000LL;
      curr_value->it_value.tv_nsec = rem_ns % 1000000000LL;
    }
  }
  return 0;
}

int BIONIC_SYSV TimerfdSettime(int fd, int flags, const LinuxItimerspec* new_value, LinuxItimerspec* old_value) {
  if (new_value == nullptr) {
    errno = kEFAULT;
    return -1;
  }
  if ((flags & ~kTfdTimerAbstime) != 0) {
    errno = kEINVAL;
    return -1;
  }
  if (new_value->it_value.tv_nsec < 0 || new_value->it_value.tv_nsec >= 1000000000LL ||
      new_value->it_interval.tv_nsec < 0 || new_value->it_interval.tv_nsec >= 1000000000LL) {
    errno = kEINVAL;
    return -1;
  }

  std::shared_ptr<TimerState> state;
  {
    std::lock_guard<std::mutex> lock(g_timer_mutex);
    auto it = g_timers.find(fd);
    if (it == g_timers.end()) {
      errno = kEBADF;
      return -1;
    }
    state = it->second;
  }

  if (old_value != nullptr) {
    TimerfdGettime(fd, old_value);
  }

  std::lock_guard<std::mutex> lock(state->mtx);
  state->spec = *new_value;

  if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0) {
    state->next_expire_ns = 0;
    state->interval_ns = 0;
    SetThreadpoolTimer(state->tp_timer, nullptr, 0, 0);
    return 0;
  }

  int64_t interval_ns = new_value->it_interval.tv_sec * 1000000000LL + new_value->it_interval.tv_nsec;
  state->interval_ns = interval_ns;

  DWORD ms_period = 0;
  if (interval_ns > 0) {
    int64_t ms = interval_ns / 1000000LL;
    ms_period = static_cast<DWORD>(ms < 1 ? 1 : ms);
  }

  int64_t delay_ns = 0;
  if ((flags & kTfdTimerAbstime) != 0) {
    int64_t now_ns = (state->clockid == kClockRealtime) ? RealtimeNowNanos() : MonotonicNowNanos();
    int64_t target_ns = new_value->it_value.tv_sec * 1000000000LL + new_value->it_value.tv_nsec;
    delay_ns = target_ns - now_ns;
    state->next_expire_ns = target_ns;
    if (delay_ns < 0) delay_ns = 0;
  } else {
    delay_ns = new_value->it_value.tv_sec * 1000000000LL + new_value->it_value.tv_nsec;
    int64_t now_ns = (state->clockid == kClockRealtime) ? RealtimeNowNanos() : MonotonicNowNanos();
    state->next_expire_ns = now_ns + delay_ns;
  }

  int64_t delay_100ns = delay_ns / 100;
  if (delay_100ns <= 0) delay_100ns = 1;
  int64_t rel_ft = -delay_100ns;

  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(rel_ft & 0xFFFFFFFF);
  ft.dwHighDateTime = static_cast<DWORD>((rel_ft >> 32) & 0xFFFFFFFF);

  SetThreadpoolTimer(state->tp_timer, &ft, ms_period, 0);
  return 0;
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
