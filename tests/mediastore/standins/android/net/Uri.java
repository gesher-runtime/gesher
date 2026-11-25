package android.net;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** The hierarchical-URI subset of android.net.Uri the media provider uses. */
public final class Uri {
    private final String mString;
    private final java.net.URI mUri;
    private Uri(String string) {
        mString = string;
        java.net.URI parsed;
        try { parsed = new java.net.URI(string); } catch (java.net.URISyntaxException e) { parsed = null; }
        mUri = parsed;
    }
    public static Uri parse(String string) { return new Uri(string); }
    public static Uri fromFile(File file) { return new Uri(file.toURI().toString().replaceFirst("^file:/+", "file:///")); }
    public String getScheme() { return mUri == null ? null : mUri.getScheme(); }
    public String getAuthority() { return mUri == null ? null : mUri.getRawAuthority(); }
    public String getPath() { return mUri == null ? null : mUri.getPath(); }
    public List<String> getPathSegments() {
        String path = getPath();
        List<String> segments = new ArrayList<>();
        if (path != null) for (String s : path.split("/")) if (!s.isEmpty()) segments.add(s);
        return Collections.unmodifiableList(segments);
    }
    public String getLastPathSegment() {
        List<String> segments = getPathSegments();
        return segments.isEmpty() ? null : segments.get(segments.size() - 1);
    }
    public String getQueryParameter(String key) {
        String query = mUri == null ? null : mUri.getRawQuery();
        if (query == null) return null;
        for (String pair : query.split("&")) {
            int eq = pair.indexOf('=');
            String name = decode(eq < 0 ? pair : pair.substring(0, eq));
            if (name.equals(key)) return eq < 0 ? "" : decode(pair.substring(eq + 1));
        }
        return null;
    }
    /** Uri.getBooleanQueryParameter: "false" and "0" (any case) are false. */
    public boolean getBooleanQueryParameter(String key, boolean defaultValue) {
        String flag = getQueryParameter(key);
        if (flag == null) return defaultValue;
        flag = flag.toLowerCase(java.util.Locale.ROOT);
        return !"false".equals(flag) && !"0".equals(flag);
    }
    public static String decode(String s) {
        if (s == null) return null;
        try { return java.net.URLDecoder.decode(s.replace("+", "%2B"), "UTF-8"); }
        catch (Exception e) { return s; }
    }
    @Override public String toString() { return mString; }
    @Override public boolean equals(Object o) { return o instanceof Uri && ((Uri) o).mString.equals(mString); }
    @Override public int hashCode() { return mString.hashCode(); }
}
