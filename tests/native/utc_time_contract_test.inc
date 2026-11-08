// Public Android ELF calls in libc_test.c; the host envelope maps only the
// types and functions to the byte-for-byte production UTC implementation.
#include <limits.h>

#ifndef UTC_TM
#define UTC_TM struct tm
#define UTC_TIME time_t
#define UTC_GMTIME_R gmtime_r
#define UTC_GMTIME gmtime
#define UTC_TIMEGM timegm
#define UTC_DIFFTIME difftime
#endif

// Keep calls observable in the optimized Android shared object.
static UTC_TM* (*volatile utc_gmtime_r)(const UTC_TIME*, UTC_TM*) = UTC_GMTIME_R;
static UTC_TM* (*volatile utc_gmtime)(const UTC_TIME*) = UTC_GMTIME;
static UTC_TIME (*volatile utc_timegm)(UTC_TM*) = UTC_TIMEGM;
static double (*volatile utc_difftime)(UTC_TIME, UTC_TIME) = UTC_DIFFTIME;

static UTC_TM utc_input(int year, int month, int day, int hour, int minute, int second) {
  UTC_TM value;
  memset(&value, 0, sizeof(value));
  value.tm_year = year;
  value.tm_mon = month;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_wday = 17;
  value.tm_yday = -91;
  value.tm_isdst = 1;
  value.tm_gmtoff = 12345;
  value.tm_zone = "input";
  return value;
}

static int utc_same_fields(const UTC_TM* a, const UTC_TM* b) {
  return a->tm_year == b->tm_year && a->tm_mon == b->tm_mon && a->tm_mday == b->tm_mday &&
      a->tm_hour == b->tm_hour && a->tm_min == b->tm_min && a->tm_sec == b->tm_sec &&
      a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday && a->tm_isdst == b->tm_isdst &&
      a->tm_gmtoff == b->tm_gmtoff;
}

static void test_utc_calendar_values(void) {
  static const struct {
    int64_t instant;
    int year, month, day, hour, minute, second, weekday, yearday;
  } cases[] = {
    {0, 70, 0, 1, 0, 0, 0, 4, 0},
    {-1, 69, 11, 31, 23, 59, 59, 3, 364},
    {-86400, 69, 11, 31, 0, 0, 0, 3, 364},
    {-2203977600LL, 0, 1, 28, 0, 0, 0, 3, 58},
    {-2203891200LL, 0, 2, 1, 0, 0, 0, 4, 59},
    {951782400, 100, 1, 29, 0, 0, 0, 2, 59},
    {4107542400LL, 200, 2, 1, 0, 0, 0, 1, 59},
    {-62167219200LL, -1900, 0, 1, 0, 0, 0, 6, 0},
    {-62198755200LL, -1901, 0, 1, 0, 0, 0, 5, 0},
    {32535216000LL, 1101, 0, 1, 0, 0, 0, 4, 0},
    {253402300799LL, 8099, 11, 31, 23, 59, 59, 5, 364},
    {-67768040609740800LL, INT_MIN, 0, 1, 0, 0, 0, 4, 0},
    {67768036191676799LL, INT_MAX, 11, 31, 23, 59, 59, 3, 364},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    UTC_TIME instant = (UTC_TIME)cases[i].instant;
    UTC_TM out = utc_input(0, 0, 0, 0, 0, 0);
    errno = EDOM;
    UTC_TM* got = utc_gmtime_r(&instant, &out);
    int fields = got == &out && out.tm_year == cases[i].year && out.tm_mon == cases[i].month &&
        out.tm_mday == cases[i].day && out.tm_hour == cases[i].hour &&
        out.tm_min == cases[i].minute && out.tm_sec == cases[i].second &&
        out.tm_wday == cases[i].weekday && out.tm_yday == cases[i].yearday;
    if (!fields) printf("UTC vector failed: instant=%lld\n", (long long)instant);
    check("gmtime_r signed Gregorian fields", fields);
    check("gmtime_r successful conversion preserves errno", got == &out && errno == EDOM);
    check("gmtime_r sets UTC offset, name and standard time",
          got == &out && out.tm_isdst == 0 && out.tm_gmtoff == 0 &&
          out.tm_zone != NULL && strcmp(out.tm_zone, "UTC") == 0);
    UTC_TM input = utc_input(cases[i].year, cases[i].month, cases[i].day,
                             cases[i].hour, cases[i].minute, cases[i].second);
    errno = EDOM;
    UTC_TIME roundtrip = utc_timegm(&input);
    check("timegm accepts full representable UTC year range", roundtrip == instant &&
          utc_same_fields(&input, &out));
    check("timegm success including minus one preserves errno", roundtrip == instant && errno == EDOM);
  }
}

static void test_utc_normalization(void) {
  static const struct {
    int year, month, day, hour, minute, second;
    int64_t instant;
  } cases[] = {
    {70, 0, 1, 0, 0, -1, -1},
    {70, -1, 32, 0, 0, 0, 0},
    {70, 12, 0, 0, 0, 0, 31449600},
    {100, 1, 30, 0, 0, 0, 951868800},
    {0, 1, 29, 0, 0, 0, -2203891200LL},
    {100, 0, 0, 0, 0, 0, 946598400},
    {100, 0, 1, -25, 0, 0, 946594800},
    {70, 0, 1, INT_MAX, -60, INT_MAX, 7733088609247LL},
    {69, 0, 1, 0, 0, INT_MIN, -2179019648LL},
    {70, 0, 1, INT_MAX, 60, -3600, 7730941129200LL},
    {INT_MAX, 12, -364, 0, 0, 0, 67768036160140800LL},
    {INT_MIN, -12, 366, 0, 0, 0, -67768040609740800LL},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    UTC_TM input = utc_input(cases[i].year, cases[i].month, cases[i].day,
                             cases[i].hour, cases[i].minute, cases[i].second);
    UTC_TIME instant = (UTC_TIME)cases[i].instant;
    UTC_TM expected;
    UTC_TM* valid = utc_gmtime_r(&instant, &expected);
    errno = EDOM;
    UTC_TIME got = utc_timegm(&input);
    if (got != instant) printf("UTC normalization failed: case=%zu got=%lld want=%lld\n",
                              i, (long long)got, (long long)instant);
    check("timegm normalizes fields with checked Android carries", got == instant && errno == EDOM);
    check("timegm replaces derived fields only after success", valid == &expected &&
          utc_same_fields(&input, &expected) && input.tm_zone != NULL &&
          strcmp(input.tm_zone, "UTC") == 0);
  }
}

static void test_utc_overflow(void) {
  const UTC_TIME outside[] = {INT64_MIN, INT64_MAX, -67768040609740801LL, 67768036191676800LL};
  for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); ++i) {
    UTC_TM out = utc_input(123, 4, 5, 6, 7, 8);
    errno = 0;
    check("gmtime_r rejects unrepresentable year with EOVERFLOW",
          utc_gmtime_r(&outside[i], &out) == NULL && errno == EOVERFLOW);
  }
  const int outside_fields[][6] = {
    {INT_MAX, 12, 1, 0, 0, 0}, {INT_MIN, -1, 1, 0, 0, 0},
    {INT_MAX, 11, 45, 0, 0, 0}, {70, 0, 1, INT_MAX, 60, 0},
    {70, 0, INT_MAX, 24, 0, 0}, {69, 0, 1, 0, INT_MIN, INT_MIN},
    // Correct overflow rejection, deliberately not tzcode's wrapped-year
    // second-attempt bug at the two endpoints. See the focused report.
    {INT_MAX, 11, 31, 23, 59, 60}, {INT_MIN, 0, 1, 0, 0, -1},
  };
  for (size_t i = 0; i < sizeof(outside_fields) / sizeof(outside_fields[0]); ++i) {
    const int* f = outside_fields[i];
    UTC_TM input = utc_input(f[0], f[1], f[2], f[3], f[4], f[5]);
    UTC_TM before = input;
    errno = 0;
    check("timegm rejects overflowing normalization with EOVERFLOW",
          utc_timegm(&input) == (UTC_TIME)-1 && errno == EOVERFLOW);
    check("timegm failure preserves the caller's entire structure",
          memcmp(&input, &before, sizeof(input)) == 0);
  }
}

static void test_utc_storage_and_difference(void) {
  UTC_TIME before_epoch = -1, epoch = 0;
  UTC_TM* first = utc_gmtime(&before_epoch);
  check("gmtime supports negative time", first != NULL && first->tm_year == 69 && first->tm_sec == 59);
  UTC_TM independent = utc_input(123, 4, 5, 6, 7, 8);
  UTC_TM* reentrant = utc_gmtime_r(&before_epoch, &independent);
  UTC_TM* second = utc_gmtime(&epoch);
  check("gmtime reuses its own storage without changing gmtime_r output",
        first != NULL && second == first && second->tm_year == 70 &&
        reentrant == &independent && independent.tm_year == 69 && independent.tm_sec == 59);
  static const struct { int64_t a, b; double difference; } differences[] = {
    {0, 0, 0}, {0, -1, 1}, {-1, 0, -1},
    {INT64_MAX, INT64_MAX - 1, 1}, {INT64_MIN + 1, INT64_MIN, 1},
    {INT64_MAX - 1, INT64_MAX, -1}, {INT64_MIN, INT64_MIN + 1, -1},
    {INT64_MAX, INT64_MIN, 0x1p64}, {INT64_MIN, INT64_MAX, -0x1p64},
    {0, INT64_MIN, 0x1p63}, {INT64_MIN, 0, -0x1p63},
    {9007199254740991LL, 0, 9007199254740991.0},
  };
  for (size_t i = 0; i < sizeof(differences) / sizeof(differences[0]); ++i)
    check("difftime keeps adjacent extremes and safely spans signs",
          utc_difftime((UTC_TIME)differences[i].a, (UTC_TIME)differences[i].b) == differences[i].difference);
#ifndef UTC_NO_ALIASES
  UTC_TM* (*gmtime64_fn)(const int64_t*, UTC_TM*) =
      (UTC_TM* (*)(const int64_t*, UTC_TM*))dlsym(RTLD_DEFAULT, "gmtime64_r");
  int64_t (*timegm64_fn)(UTC_TM*) = (int64_t (*)(UTC_TM*))dlsym(RTLD_DEFAULT, "timegm64");
  int64_t instant = -1;
  UTC_TM out;
  int converted = gmtime64_fn != NULL && gmtime64_fn(&instant, &out) == &out;
  check("gmtime64_r public alias supports pre-epoch dates",
        converted && out.tm_year == 69 && out.tm_sec == 59);
  errno = EDOM;
  check("timegm64 public alias preserves legitimate minus one",
        converted && timegm64_fn != NULL && timegm64_fn(&out) == -1 && errno == EDOM);
#endif
}

static void test_utc_time_contracts(void) {
  test_utc_calendar_values();
  test_utc_normalization();
  test_utc_overflow();
  test_utc_storage_and_difference();
}
