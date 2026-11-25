package android.webkit;
/** The few MimeMap entries the fixtures use. */
public final class MimeTypeMap {
    private static final MimeTypeMap INSTANCE = new MimeTypeMap();
    private static final java.util.Map<String, String> TYPES = java.util.Map.of(
            "bin", "application/octet-stream", "txt", "text/plain", "html", "text/html",
            "zip", "application/zip", "png", "image/png");
    public static MimeTypeMap getSingleton() { return INSTANCE; }
    public String getMimeTypeFromExtension(String extension) { return TYPES.get(extension.toLowerCase()); }
    public String getExtensionFromMimeType(String mimeType) {
        for (java.util.Map.Entry<String, String> e : TYPES.entrySet()) if (e.getValue().equals(mimeType)) return e.getKey();
        return null;
    }
}
