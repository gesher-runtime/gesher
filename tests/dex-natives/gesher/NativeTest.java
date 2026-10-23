package gesher;

import android.app.Activity;
import android.os.Process;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.Trace;
import android.text.AndroidCharacter;
import android.util.EventLog;
import android.util.Log;

/**
 * Calls into libandroid_runtime's registered natives (step 2 of
 * docs/framework-natives.md). The framework classes here are stand-ins with
 * the same names and native signatures; loading "android_runtime" resolves to
 * the built-in library and registers on them. Prints "all passed" and exits 0
 * when every check holds.
 */
public final class NativeTest {
  private static int fails = 0;

  private static void check(boolean ok, String what) {
    if (!ok) {
      System.out.println("FAIL: " + what);
      fails++;
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");

    // SystemClock: @CriticalNative over libutils' clocks.
    long up0 = SystemClock.uptimeMillis();
    check(up0 > 0, "uptimeMillis > 0");
    check(SystemClock.uptimeNanos() >= up0 * 1000000L, "uptimeNanos consistent");
    check(SystemClock.elapsedRealtime() >= up0, "elapsedRealtime >= uptimeMillis");
    check(SystemClock.elapsedRealtimeNanos() > 0, "elapsedRealtimeNanos > 0");
    check(SystemClock.currentTimeMicro() > 1_600_000_000_000_000L, "currentTimeMicro is epoch us");
    Thread.sleep(20);
    check(SystemClock.uptimeMillis() - up0 >= 15, "uptimeMillis advances");
    check(SystemClock.currentThreadTimeMicro() >= 0, "currentThreadTimeMicro");
    check(SystemClock.currentThreadTimeMillis() >= 0, "currentThreadTimeMillis");

    // SystemProperties: libbase's in-process store, string and handle paths.
    SystemProperties.native_set("gesher.test.value", "1234");
    check("1234".equals(SystemProperties.native_get("gesher.test.value", "")),
        "native_get returns the set value");
    check(SystemProperties.native_get_int("gesher.test.value", 0) == 1234, "native_get_int");
    check(SystemProperties.native_get_long("gesher.test.value", 0) == 1234L, "native_get_long");
    SystemProperties.native_set("gesher.test.flag", "true");
    check(SystemProperties.native_get_boolean("gesher.test.flag", false), "native_get_boolean");
    long handle = SystemProperties.native_find("gesher.test.value");
    check(handle != 0, "native_find");
    check("1234".equals(SystemProperties.native_get(handle)), "native_get by handle");
    check(SystemProperties.native_get_int(handle, 0) == 1234, "native_get_int by handle (critical)");
    check(SystemProperties.native_get_long(handle, 0) == 1234L,
        "native_get_long by handle (critical)");
    check(SystemProperties.native_find("gesher.test.missing") == 0, "native_find missing");
    SystemProperties.native_add_change_callback();
    SystemProperties.native_report_sysprop_change();

    // Log: liblog on the host (writes go to stderr).
    check(Log.maxPayload() > 0, "logger_entry_max_payload_native > 0");
    check(Log.isLoggable("GesherTest", Log.INFO), "isLoggable(INFO)");
    check(!Log.isLoggable("GesherTest", Log.VERBOSE), "not isLoggable(VERBOSE)");
    check(Log.println_native(0, Log.INFO, "GesherTest", "hello from Log") >= 0, "println_native");

    // EventLog: the writers return; reading needs no logd and returns nothing.
    EventLog.writeEvent(314, 42);
    EventLog.writeEvent(314, 42L);
    EventLog.writeEvent(314, "gesher");

    // Trace: perfetto is stubbed with every tag off.
    check(!Trace.nativeIsTagEnabled(Trace.TRACE_TAG_ALWAYS), "no trace tag is on (critical)");
    check(!Trace.nativeIsTagEnabled(Trace.TRACE_TAG_APP), "app trace tag is off");
    Trace.nativeSetAppTracingAllowed(true);
    Trace.nativeSetTracingEnabled(true);
    Trace.nativeTraceBegin(Trace.TRACE_TAG_APP, "gesher-section");
    Trace.nativeTraceEnd(Trace.TRACE_TAG_APP);
    Trace.nativeTraceCounter(Trace.TRACE_TAG_APP, "gesher-counter", 7);
    Trace.nativeAsyncTraceBegin(Trace.TRACE_TAG_APP, "gesher-async", 1);
    Trace.nativeAsyncTraceEnd(Trace.TRACE_TAG_APP, "gesher-async", 1);
    Trace.nativeInstant(Trace.TRACE_TAG_APP, "gesher-instant");
    Trace.nativeInstantForTrack(Trace.TRACE_TAG_APP, "track", null);  // null name -> "(null)"

    // AndroidCharacter: ICU4C.
    check(AndroidCharacter.getMirror('(') == ')', "getMirror mirrors parentheses");
    check(AndroidCharacter.getMirror('A') == 'A', "getMirror keeps letters");
    char[] mirrored = {'(', '[', 'x'};
    check(AndroidCharacter.mirror(mirrored, 0, 3), "mirror finds mirrorable chars");
    check(mirrored[0] == ')' && mirrored[1] == ']' && mirrored[2] == 'x', "mirror in place");
    char[] text = {'A', '\u05d0', '1'};  // latin, hebrew alef, digit
    byte[] dirs = new byte[3];
    AndroidCharacter.getDirectionalities(text, dirs, 3);
    check(dirs[0] == Character.DIRECTIONALITY_LEFT_TO_RIGHT, "latin is L");
    check(dirs[1] == Character.DIRECTIONALITY_RIGHT_TO_LEFT, "hebrew is R");
    byte[] widths = new byte[1];
    AndroidCharacter.getEastAsianWidths(new char[] {'A'}, 0, 1, widths);
    check(AndroidCharacter.getEastAsianWidth('A') == widths[0], "east asian widths agree");

    // Process and Activity: the Gesher-owned stubs.
    Process.setThreadPriority(0);
    Process.setThreadPriority(0, 0);
    Process.setArgV0("gesher-native-test");
    check(Process.getThreadPriority(0) == 0, "getThreadPriority");
    check(Process.getElapsedCpuTime() >= 0, "getElapsedCpuTime");
    Process.setThreadGroup(0, Process.THREAD_GROUP_DEFAULT);
    check(Process.getProcessGroup(0) == Process.THREAD_GROUP_DEFAULT, "getProcessGroup");
    check(Process.getUidForName("root") == -1, "getUidForName");
    check(new Activity().getDlWarning() == null, "getDlWarning is null");

    // The @CriticalNative ABI canary: a System V / Microsoft mismatch in how
    // ART calls the natives would scramble the arguments.
    check(NativeCanary.ints(1, 2, 3, 4, 5, 6, 7, 8, 9L, 10L)
            == NativeCanary.intsExpected(1, 2, 3, 4, 5, 6, 7, 8, 9L, 10L),
        "canary ints");
    check(NativeCanary.ints(-7, 1 << 20, 3, -4, 5, 60, -700, 8000, 1L << 40, -(1L << 33))
            == NativeCanary.intsExpected(-7, 1 << 20, 3, -4, 5, 60, -700, 8000, 1L << 40,
                -(1L << 33)),
        "canary ints, wide values");
    check(NativeCanary.floats(1f, 2, 3f, 4, 5f, 6, 7f, 8, 9f)
            == NativeCanary.floatsExpected(1f, 2, 3f, 4, 5f, 6, 7f, 8, 9f),
        "canary floats");
    check(NativeCanary.floats(0.5f, -2, 3.25f, 4, -5f, 6.5, 7f, -8, 9.75f)
            == NativeCanary.floatsExpected(0.5f, -2, 3.25f, 4, -5f, 6.5, 7f, -8, 9.75f),
        "canary floats, mixed signs");
    check(NativeCanary.mixed(1, 2f, 3L, 4, 5, 6f, 7L, 8)
            == NativeCanary.mixedExpected(1, 2f, 3L, 4, 5, 6f, 7L, 8),
        "canary mixed");
    check(NativeCanary.mixed(-1, 0.5f, 1L << 35, -4.25, 50, -6f, -7L, 8.125)
            == NativeCanary.mixedExpected(-1, 0.5f, 1L << 35, -4.25, 50, -6f, -7L, 8.125),
        "canary mixed, wide values");

    System.out.println(fails == 0 ? "all passed" : fails + " failures");
    System.exit(fails == 0 ? 0 : 1);
  }
}
