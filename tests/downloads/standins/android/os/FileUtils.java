package android.os;
/** android.os.FileUtils.buildValidFatFilename and its helpers, as in android-16.0.0_r4. */
public final class FileUtils {
    private FileUtils() {}
    private static boolean isValidFatFilenameChar(char c) {
        if ((0x00 <= c && c <= 0x1f)) return false;
        switch (c) {
            case '"': case '*': case '/': case ':': case '<': case '>': case '?': case '\\': case '|': case 0x7F:
                return false;
            default:
                return true;
        }
    }
    public static String buildValidFatFilename(String name) {
        if (name == null || name.isEmpty() || ".".equals(name) || "..".equals(name)) return "(invalid)";
        final StringBuilder res = new StringBuilder(name.length());
        for (int i = 0; i < name.length(); i++) {
            final char c = name.charAt(i);
            if (isValidFatFilenameChar(c)) res.append(c); else res.append('_');
        }
        trimFilename(res, 255);
        return res.toString();
    }
    private static void trimFilename(StringBuilder res, int maxBytes) {
        byte[] raw = res.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (raw.length > maxBytes) {
            maxBytes -= 3;
            while (raw.length > maxBytes) {
                res.deleteCharAt(res.length() / 2);
                raw = res.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
            }
            res.insert(res.length() / 2, "...");
        }
    }
}
