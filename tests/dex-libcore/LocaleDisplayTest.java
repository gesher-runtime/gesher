package gesher;

import java.nio.charset.Charset;
import java.text.Collator;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Locale's display names (java.util.Locale -> com.android.icu.util.LocaleNative ->
 * libicu_jni) are never null, whatever native code ran before on the thread.
 *
 * libicu_jni's ScopedIcuLocale hands ICU an uninitialized UErrorCode; Android's
 * build zeroes uninitialized locals (Soong: -ftrivial-auto-var-init=zero) and so
 * must Gesher's (cmake/aosp/icu.cmake). Without it, Locale.getDisplayCountry()
 * returned null when the stack held a failure code (Breezy Weather crashed in
 * its MainActivity.onCreate on new Locale.Builder().setRegion("ES")...
 * .getDisplayCountry()). The test runs the display names after natives that
 * leave ICU error codes and other values on the stack (unknown charsets,
 * collators, ill-formed tags), from frames of several depths.
 *
 *   LocaleDisplayTest
 *
 * Prints "LocaleDisplayTest all passed".
 */
public final class LocaleDisplayTest {
  private static final String[] DISPLAY = {"en-US", "he-IL", "fr-FR", "ar-EG", "ja-JP", "und"};
  private static final List<String> problems = new ArrayList<>();
  private static int checks;

  public static void main(String[] args) {
    // Known values first (the ICU data's region and language names).
    expect("en-ES in English", new Locale.Builder().setLanguage("en").setRegion("ES").build()
        .getDisplayCountry(Locale.ENGLISH), "Spain");
    expect("he-ES in Hebrew", new Locale.Builder().setLanguage("he").setRegion("ES").build()
        .getDisplayCountry(Locale.forLanguageTag("he-IL")), "\u05e1\u05e4\u05e8\u05d3");
    expect("fr in English", Locale.FRENCH.getDisplayLanguage(Locale.ENGLISH), "French");
    expect("zh-Hant in English", Locale.forLanguageTag("zh-Hant-TW").getDisplayScript(Locale.ENGLISH),
        "Traditional Han");

    for (int round = 0; round < 60; round++) {
      for (int depth = 0; depth < 24; depth++) {
        nested(depth, round);
      }
    }
    if (!problems.isEmpty()) {
      for (String p : problems.subList(0, Math.min(20, problems.size()))) System.out.println("FAIL " + p);
      throw new AssertionError(problems.size() + " of " + checks + " display names were null or wrong");
    }
    System.out.println("display names " + checks);
    System.out.println("LocaleDisplayTest all passed");
  }

  /** Natives that leave ICU failure codes (and other values) on the native stack. */
  private static void dirty(int round) {
    try {
      Charset.forName("x-gesher-no-such-charset-" + round);
    } catch (IllegalArgumentException expected) {
      // UnsupportedCharsetException: ICU's ucnv_open failed.
    }
    Charset.isSupported("windows-1255");
    Collator.getInstance(Locale.forLanguageTag(DISPLAY[round % DISPLAY.length])).compare("a", "b");
    Locale.forLanguageTag("x-" + round).getDisplayName();
    "\u0130stanbul".toLowerCase(Locale.forLanguageTag("tr"));
  }

  private static void nested(int depth, int round) {
    if (depth > 0) {
      nested(depth - 1, round);
      return;
    }
    dirty(round);
    Locale display = Locale.forLanguageTag(DISPLAY[round % DISPLAY.length]);
    Locale.setDefault(display);
    Locale target = new Locale.Builder().setLanguage(display.getLanguage().isEmpty() ? "en"
        : display.getLanguage()).setRegion("ES").build();
    Locale zh = Locale.forLanguageTag("zh-Hant");
    Locale de = Locale.forLanguageTag("de-DE-1996");
    notNull("getDisplayCountry() of " + target + " in " + display, target::getDisplayCountry);
    notNull("getDisplayCountry(" + display + ") of " + target, () -> target.getDisplayCountry(display));
    notNull("getDisplayLanguage() of " + target, target::getDisplayLanguage);
    notNull("getDisplayScript() of zh-Hant", zh::getDisplayScript);
    notNull("getDisplayVariant() of de-DE-1996", de::getDisplayVariant);
    notNull("getDisplayName() of " + target, target::getDisplayName);
  }

  private interface Name {
    String get();
  }

  /** The display name is not null (nor an exception: libcore dereferences a null one). */
  private static void notNull(String what, Name name) {
    checks++;
    try {
      if (name.get() == null) problems.add(what + " is null");
    } catch (RuntimeException e) {
      problems.add(what + ": " + e);
    }
  }

  private static void expect(String what, String value, String want) {
    checks++;
    if (!want.equals(value)) problems.add(what + ": '" + value + "', expected '" + want + "'");
  }
}
