package android.content;
import android.net.Uri;
public final class ContentUris {
    private ContentUris() {}
    public static Uri withAppendedId(Uri contentUri, long id) {
        return Uri.parse(contentUri.toString() + "/" + id);
    }
    public static long parseId(Uri uri) {
        String last = uri.getLastPathSegment();
        return last == null ? -1 : Long.parseLong(last);
    }
}
