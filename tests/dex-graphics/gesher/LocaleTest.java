package gesher;

import android.os.LocaleList;
import com.gesher.app.locale.SystemLocale;
import java.text.NumberFormat;
import java.util.Calendar;
import java.util.Locale;

/**
 * The device's locales from Windows (com.gesher.app.locale.SystemLocale; docs/fonts.md,
 * ADR 0044), against what the test script read from Windows itself:
 *
 *   gesher.LocaleTest LOCALES FIRST_DAY 24H [NUMBER]
 *
 * LOCALES is the expected locale list (language tags, ',' between), FIRST_DAY the first
 * day of the week as java.util.Calendar numbers it (1 = Sunday, 2 = Monday), 24H
 * {@code true} or {@code false}, NUMBER (optional) 1234567.5 as the first locale formats
 * it. After SystemLocale.applyDefault, Locale.getDefault and LocaleList.getDefault are the
 * list's, and Calendar and NumberFormat follow its regional preferences. Regional
 * preferences (-u-) in LOCALES are compared only when LOCALES has some; with
 * $GESHER_LOCALES the locale's own time pattern is not compared with Windows'.
 */
public final class LocaleTest {
  private static int failures;

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    String expected = args[0];
    int firstDay = Integer.parseInt(args[1]);
    boolean is24 = Boolean.parseBoolean(args[2]);
    String number = args.length > 3 ? args[3] : null;

    String describe = SystemLocale.describe();
    LocaleList list = SystemLocale.locales();
    // Regional preferences (-u-) are compared only when the expected list has them.
    String actual = list.toLanguageTags();
    if (!expected.contains("-u-")) {
      StringBuilder plain = new StringBuilder();
      for (int i = 0; i < list.size(); i++) {
        if (i > 0) plain.append(',');
        plain.append(list.get(i).stripExtensions().toLanguageTag());
      }
      actual = plain.toString();
    }
    check(actual.equals(expected), "locales " + describe + " (expected " + expected + ")");
    boolean fromWindows = !describe.contains("$" + SystemLocale.ENV);
    SystemLocale.applyDefault();
    check(Locale.getDefault().equals(list.get(0))
            && LocaleList.getDefault().toLanguageTags().equals(list.toLanguageTags()),
        "default Locale " + Locale.getDefault().toLanguageTag() + " and LocaleList "
            + LocaleList.getDefault().toLanguageTags());
    int day = Calendar.getInstance().getFirstDayOfWeek();
    int icuDay = android.icu.util.Calendar.getInstance().getFirstDayOfWeek();
    check(day == firstDay && icuDay == firstDay,
        "first day of the week " + day + " (ICU " + icuDay + ", Windows " + firstDay + ")");
    check(SystemLocale.is24Hour() == is24,
        "24-hour time " + SystemLocale.is24Hour() + " (Windows " + is24 + ")");
    String time = new java.text.SimpleDateFormat(
        android.text.format.DateFormat.getBestDateTimePattern(Locale.getDefault(), "jm"),
        Locale.getDefault()).toPattern();
    check(!fromWindows || (time.indexOf('H') >= 0 || time.indexOf('k') >= 0) == is24,
        "the locale's time pattern " + time + " follows (24-hour " + is24 + ")");
    String formatted = NumberFormat.getInstance().format(1234567.5);
    check(number == null || number.equals(formatted),
        "numbers: 1234567.5 is " + formatted + (number != null ? " (Windows " + number + ")" : ""));
    if (failures == 0) {
      System.out.println("LocaleTest all passed");
    } else {
      System.out.println("LocaleTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) failures++;
  }
}
