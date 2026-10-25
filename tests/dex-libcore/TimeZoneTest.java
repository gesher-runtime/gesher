package gesher;

import java.lang.reflect.Method;
import java.time.Instant;
import java.time.ZoneId;
import java.time.zone.ZoneRules;
import java.util.TimeZone;

/**
 * Time zones on Gesher (docs/posix.md): the default zone is Windows' zone
 * (libcore reads /etc/timezone, which gesher-base serves), the tzdata module's
 * files are loaded, and java.util.TimeZone, java.time and android.icu give the
 * same offsets.
 *
 *   TimeZoneTest <expected default id or -> [epoch millis...]
 *
 * Prints "default <id>" and "offset <millis> <offset ms>" for each instant,
 * checked by tests/windows/tracks/21-posix.ps1 against Windows' own offsets;
 * then "TimeZoneTest all passed".
 */
public final class TimeZoneTest {
  public static void main(String[] args) throws Exception {
    TimeZone zone = TimeZone.getDefault();
    String id = zone.getID();
    System.out.println("default " + id);
    if (!args[0].equals("-")) check(id.equals(args[0]), "default zone " + id + ", expected " + args[0]);
    check(ZoneId.systemDefault().getId().equals(id), "java.time's default " + ZoneId.systemDefault());

    // The tzdata module's data: every IANA zone, not only GMT.
    check(TimeZone.getAvailableIDs().length > 400, "available ids: " + TimeZone.getAvailableIDs().length);
    check(TimeZone.getTimeZone("Asia/Jerusalem").getID().equals("Asia/Jerusalem"), "Asia/Jerusalem known");
    check(ZoneId.of("America/New_York").getRules().isDaylightSavings(Instant.parse("2026-07-01T12:00:00Z")),
        "java.time: New York in DST in July");

    Class<?> icuZone = Class.forName("android.icu.util.TimeZone");
    Object icuDefault = icuZone.getMethod("getDefault").invoke(null);
    String icuId = (String) icuZone.getMethod("getID").invoke(icuDefault);
    check(icuId.equals(id), "android.icu's default " + icuId);
    Method icuOffset = icuZone.getMethod("getOffset", long.class);
    ZoneRules rules = ZoneId.systemDefault().getRules();
    for (int i = 1; i < args.length; i++) {
      long t = Long.parseLong(args[i]);
      int util = zone.getOffset(t);
      int time = rules.getOffset(Instant.ofEpochMilli(t)).getTotalSeconds() * 1000;
      int icu = (Integer) icuOffset.invoke(icuDefault, t);
      check(util == time && time == icu,
          "offsets at " + t + ": java.util " + util + ", java.time " + time + ", android.icu " + icu);
      System.out.println("offset " + t + " " + util);
    }
    System.out.println("TimeZoneTest all passed");
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
