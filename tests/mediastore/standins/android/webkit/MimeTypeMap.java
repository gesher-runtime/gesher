package android.webkit;
/** A subset of libcore's MimeMap (android-16.0.0_r4 mime.types) for the fixtures' types. */
public final class MimeTypeMap {
    private static final MimeTypeMap INSTANCE = new MimeTypeMap();
    // Extension -> MIME type; the first extension listed for a type is its preferred one.
    private static final String[][] TYPES = {
            {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
            {"gif", "image/gif"}, {"webp", "image/webp"}, {"bmp", "image/bmp"},
            {"heic", "image/heic"}, {"mp4", "video/mp4"}, {"3gp", "video/3gpp"},
            {"webm", "video/webm"}, {"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"},
            {"m4a", "audio/mp4"}, {"wav", "audio/x-wav"}, {"m3u", "audio/x-mpegurl"},
            {"srt", "application/x-subrip"}, {"txt", "text/plain"}, {"pdf", "application/pdf"},
            {"zip", "application/zip"}, {"bin", "application/octet-stream"},
    };
    public static MimeTypeMap getSingleton() { return INSTANCE; }
    public String getMimeTypeFromExtension(String extension) {
        if (extension == null) return null;
        for (String[] t : TYPES) if (t[0].equals(extension.toLowerCase())) return t[1];
        return null;
    }
    public String getExtensionFromMimeType(String mimeType) {
        if (mimeType == null) return null;
        for (String[] t : TYPES) if (t[1].equals(mimeType.toLowerCase())) return t[0];
        return null;
    }
}
