package android.provider;

import android.net.Uri;

/** android.provider.Downloads.Impl constants, android-16.0.0_r4. */
public final class Downloads {
    private Downloads() {}
    public static final class Impl implements BaseColumns {
        public static final String AUTHORITY = "downloads";
        public static final String PERMISSION_ACCESS = "android.permission.ACCESS_DOWNLOAD_MANAGER";
        public static final String PERMISSION_ACCESS_ADVANCED = "android.permission.ACCESS_DOWNLOAD_MANAGER_ADVANCED";
        public static final String PERMISSION_ACCESS_ALL = "android.permission.ACCESS_ALL_DOWNLOADS";
        public static final String PERMISSION_CACHE_NON_PURGEABLE = "android.permission.DOWNLOAD_CACHE_NON_PURGEABLE";
        public static final String PERMISSION_NO_NOTIFICATION = "android.permission.DOWNLOAD_WITHOUT_NOTIFICATION";
        public static final Uri CONTENT_URI = Uri.parse("content://downloads/my_downloads");
        public static final Uri ALL_DOWNLOADS_CONTENT_URI = Uri.parse("content://downloads/all_downloads");
        public static final String ACTION_DOWNLOAD_COMPLETED = "android.intent.action.DOWNLOAD_COMPLETED";
        public static final String COLUMN_URI = "uri";
        public static final String COLUMN_APP_DATA = "entity";
        public static final String COLUMN_NO_INTEGRITY = "no_integrity";
        public static final String COLUMN_FILE_NAME_HINT = "hint";
        public static final String _DATA = "_data";
        public static final String COLUMN_MIME_TYPE = "mimetype";
        public static final String COLUMN_DESTINATION = "destination";
        public static final String COLUMN_VISIBILITY = "visibility";
        public static final String COLUMN_CONTROL = "control";
        public static final String COLUMN_STATUS = "status";
        public static final String COLUMN_LAST_MODIFICATION = "lastmod";
        public static final String COLUMN_NOTIFICATION_PACKAGE = "notificationpackage";
        public static final String COLUMN_NOTIFICATION_CLASS = "notificationclass";
        public static final String COLUMN_NOTIFICATION_EXTRAS = "notificationextras";
        public static final String COLUMN_COOKIE_DATA = "cookiedata";
        public static final String COLUMN_USER_AGENT = "useragent";
        public static final String COLUMN_REFERER = "referer";
        public static final String COLUMN_TOTAL_BYTES = "total_bytes";
        public static final String COLUMN_CURRENT_BYTES = "current_bytes";
        public static final String COLUMN_OTHER_UID = "otheruid";
        public static final String COLUMN_TITLE = "title";
        public static final String COLUMN_DESCRIPTION = "description";
        public static final String COLUMN_IS_PUBLIC_API = "is_public_api";
        public static final String COLUMN_ALLOWED_NETWORK_TYPES = "allowed_network_types";
        public static final String COLUMN_ALLOW_ROAMING = "allow_roaming";
        public static final String COLUMN_ALLOW_METERED = "allow_metered";
        public static final String COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI = "is_visible_in_downloads_ui";
        public static final String COLUMN_BYPASS_RECOMMENDED_SIZE_LIMIT = "bypass_recommended_size_limit";
        public static final String COLUMN_DELETED = "deleted";
        public static final String COLUMN_MEDIAPROVIDER_URI = "mediaprovider_uri";
        public static final String COLUMN_MEDIASTORE_URI = "mediastore_uri";
        public static final String COLUMN_MEDIA_SCANNED = "scanned";
        public static final int MEDIA_NOT_SCANNED = 0, MEDIA_SCANNED = 1, MEDIA_NOT_SCANNABLE = 2;
        public static final String COLUMN_ERROR_MSG = "errorMsg";
        public static final String COLUMN_FAILED_CONNECTIONS = "numfailed";
        public static final String COLUMN_ALLOW_WRITE = "allow_write";
        public static final String COLUMN_FLAGS = "flags";
        public static final int FLAG_REQUIRES_CHARGING = 1 << 0;
        public static final int FLAG_REQUIRES_DEVICE_IDLE = 1 << 1;
        public static final int DESTINATION_EXTERNAL = 0, DESTINATION_CACHE_PARTITION = 1,
                DESTINATION_CACHE_PARTITION_PURGEABLE = 2, DESTINATION_CACHE_PARTITION_NOROAMING = 3,
                DESTINATION_FILE_URI = 4, DESTINATION_SYSTEMCACHE_PARTITION = 5,
                DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD = 6;
        public static final int CONTROL_RUN = 0, CONTROL_PAUSED = 1;
        public static boolean isStatusSuccess(int status) { return status >= 200 && status < 300; }
        public static boolean isStatusError(int status) { return status >= 400 && status < 600; }
        public static boolean isStatusCompleted(int status) {
            return (status >= 200 && status < 300) || (status >= 400 && status < 600);
        }
        public static final int STATUS_PENDING = 190, STATUS_RUNNING = 192, STATUS_PAUSED_BY_APP = 193,
                STATUS_WAITING_TO_RETRY = 194, STATUS_WAITING_FOR_NETWORK = 195,
                STATUS_QUEUED_FOR_WIFI = 196, STATUS_INSUFFICIENT_SPACE_ERROR = 198,
                STATUS_DEVICE_NOT_FOUND_ERROR = 199, STATUS_SUCCESS = 200, STATUS_BAD_REQUEST = 400,
                STATUS_NOT_ACCEPTABLE = 406, STATUS_LENGTH_REQUIRED = 411,
                STATUS_PRECONDITION_FAILED = 412, STATUS_FILE_ALREADY_EXISTS_ERROR = 488,
                STATUS_CANNOT_RESUME = 489, STATUS_CANCELED = 490, STATUS_UNKNOWN_ERROR = 491,
                STATUS_FILE_ERROR = 492, STATUS_UNHANDLED_REDIRECT = 493,
                STATUS_UNHANDLED_HTTP_CODE = 494, STATUS_HTTP_DATA_ERROR = 495,
                STATUS_HTTP_EXCEPTION = 496, STATUS_TOO_MANY_REDIRECTS = 497, STATUS_BLOCKED = 498;
        public static final int VISIBILITY_VISIBLE = 0, VISIBILITY_VISIBLE_NOTIFY_COMPLETED = 1,
                VISIBILITY_HIDDEN = 2;
        public static final class RequestHeaders {
            public static final String HEADERS_DB_TABLE = "request_headers";
            public static final String COLUMN_DOWNLOAD_ID = "download_id";
            public static final String COLUMN_HEADER = "header";
            public static final String COLUMN_VALUE = "value";
            public static final String URI_SEGMENT = "headers";
            public static final String INSERT_KEY_PREFIX = "http_header_";
        }
    }
}
