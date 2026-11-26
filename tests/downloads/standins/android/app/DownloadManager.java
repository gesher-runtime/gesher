package android.app;

import android.provider.Downloads;

/** android.app.DownloadManager constants, android-16.0.0_r4. */
public final class DownloadManager {
    private DownloadManager() {}
    public static final String COLUMN_ID = Downloads.Impl._ID;
    public static final String COLUMN_TITLE = Downloads.Impl.COLUMN_TITLE;
    public static final String COLUMN_DESCRIPTION = Downloads.Impl.COLUMN_DESCRIPTION;
    public static final String COLUMN_URI = Downloads.Impl.COLUMN_URI;
    public static final String COLUMN_MEDIA_TYPE = "media_type";
    public static final String COLUMN_TOTAL_SIZE_BYTES = "total_size";
    public static final String COLUMN_LOCAL_URI = "local_uri";
    public static final String COLUMN_LOCAL_FILENAME = "local_filename";
    public static final String COLUMN_STATUS = Downloads.Impl.COLUMN_STATUS;
    public static final String COLUMN_FILE_NAME_HINT = Downloads.Impl.COLUMN_FILE_NAME_HINT;
    public static final String COLUMN_REASON = "reason";
    public static final String COLUMN_BYTES_DOWNLOADED_SO_FAR = "bytes_so_far";
    public static final String COLUMN_LAST_MODIFIED_TIMESTAMP = "last_modified_timestamp";
    public static final String COLUMN_MEDIAPROVIDER_URI = Downloads.Impl.COLUMN_MEDIAPROVIDER_URI;
    public static final String COLUMN_DESTINATION = Downloads.Impl.COLUMN_DESTINATION;
    public static final String COLUMN_MEDIASTORE_URI = Downloads.Impl.COLUMN_MEDIASTORE_URI;
    public static final String COLUMN_ALLOW_WRITE = Downloads.Impl.COLUMN_ALLOW_WRITE;
    public static final String ACTION_DOWNLOAD_COMPLETE = "android.intent.action.DOWNLOAD_COMPLETE";
    public static final String EXTRA_DOWNLOAD_ID = "extra_download_id";
    public static final String[] UNDERLYING_COLUMNS = new String[] {
        COLUMN_ID, COLUMN_LOCAL_FILENAME, COLUMN_MEDIAPROVIDER_URI, COLUMN_DESTINATION, COLUMN_TITLE,
        COLUMN_DESCRIPTION, COLUMN_URI, COLUMN_STATUS, COLUMN_FILE_NAME_HINT, COLUMN_MEDIA_TYPE,
        COLUMN_TOTAL_SIZE_BYTES, COLUMN_LAST_MODIFIED_TIMESTAMP, COLUMN_BYTES_DOWNLOADED_SO_FAR,
        COLUMN_ALLOW_WRITE, COLUMN_LOCAL_URI, COLUMN_REASON
    };
    public static class Request {
        public static final int NETWORK_MOBILE = 1 << 0;
        public static final int NETWORK_WIFI = 1 << 1;
        public static final int VISIBILITY_VISIBLE = 0;
        public static final int VISIBILITY_VISIBLE_NOTIFY_COMPLETED = 1;
        public static final int VISIBILITY_HIDDEN = 2;
        public static final int VISIBILITY_VISIBLE_NOTIFY_ONLY_COMPLETION = 3;
    }
}
