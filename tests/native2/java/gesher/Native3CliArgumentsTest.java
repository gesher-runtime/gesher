package gesher;

/** Numeric CLI receipts distinguish UTF-16/UTF-8 transport from console rendering. */
public final class Native3CliArgumentsTest {
    private static String codePoints(String text) {
        StringBuilder result = new StringBuilder();
        for (int at = 0; at < text.length();) {
            int value = text.codePointAt(at);
            if (result.length() != 0) result.append(',');
            result.append(Integer.toHexString(value));
            at += Character.charCount(value);
        }
        return result.toString();
    }
    public static void main(String[] args) {
        String[] expected = {"\u05d0\u05d1", "word with space", "quote\"inside", "tail\\\\", "\ud83d\ude00", ""};
        System.out.println("FONT_CLI_ARG_COUNT " + args.length);
        boolean same = args.length == expected.length;
        for (int i = 0; i < args.length; ++i) {
            String want = i < expected.length ? codePoints(expected[i]) : "(extra)";
            System.out.println("FONT_CLI_ARG index=" + i + " utf16_units=" + args[i].length()
                    + " codepoints=" + codePoints(args[i]) + " expected=" + want);
            if (i >= expected.length || !args[i].equals(expected[i])) same = false;
        }
        if (!same) throw new IllegalStateException("CLI Unicode/quoting arguments changed");
        System.out.println("FONT_CLI_UNICODE_PASS hebrew spaces quote backslashes supplementary empty");
    }
}
