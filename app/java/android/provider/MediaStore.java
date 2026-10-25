/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.provider;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Size;

import java.io.File;
import java.util.Collections;
import java.util.List;
import java.util.Set;

/**
 * A placeholder for the MediaProvider module's MediaStore (framework.jar compiles
 * against the module's API stubs and does not contain it). The framework calls it
 * where it touches media files: MediaPlayer.setDataSource(FileDescriptor, ...)
 * goes through FileUtils.convertToModernFd, which asks
 * getOriginalMediaFormatFileDescriptor for a transcoding-free descriptor (docs/media.md).
 * Gesher has no media provider: there is nothing to transcode, nothing is
 * scanned, and the content URIs name collections no provider answers. When a module jar
 * on the boot class path brings the real class, it wins.
 */
public final class MediaStore {
    public static final String AUTHORITY = "media";
    public static final Uri AUTHORITY_URI = Uri.parse("content://" + AUTHORITY);
    public static final String VOLUME_INTERNAL = "internal";
    public static final String VOLUME_EXTERNAL = "external";
    public static final String VOLUME_EXTERNAL_PRIMARY = "external_primary";

    public static final String ACTION_IMAGE_CAPTURE = "android.media.action.IMAGE_CAPTURE";
    public static final String ACTION_IMAGE_CAPTURE_SECURE = "android.media.action.IMAGE_CAPTURE_SECURE";
    public static final String ACTION_VIDEO_CAPTURE = "android.media.action.VIDEO_CAPTURE";
    public static final String ACTION_PICK_IMAGES = "android.provider.action.PICK_IMAGES";
    public static final String INTENT_ACTION_MEDIA_SEARCH = "android.intent.action.MEDIA_SEARCH";
    public static final String INTENT_ACTION_MUSIC_PLAYER = "android.intent.action.MUSIC_PLAYER";
    public static final String INTENT_ACTION_STILL_IMAGE_CAMERA = "android.media.action.STILL_IMAGE_CAMERA";
    public static final String INTENT_ACTION_VIDEO_CAMERA = "android.media.action.VIDEO_CAMERA";
    public static final String EXTRA_OUTPUT = "output";
    public static final String EXTRA_DURATION_LIMIT = "android.intent.extra.durationLimit";
    public static final String EXTRA_SIZE_LIMIT = "android.intent.extra.sizeLimit";
    public static final String EXTRA_VIDEO_QUALITY = "android.intent.extra.videoQuality";
    public static final String EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX";
    public static final String UNKNOWN_STRING = "<unknown>";

    private MediaStore() {
    }

    /** No transcoding without a media provider: null, the caller keeps its descriptor. */
    public static ParcelFileDescriptor getOriginalMediaFormatFileDescriptor(Context context,
            ParcelFileDescriptor fileDescriptor) {
        return null;
    }

    /** The volume a media URI names: its first path segment (content://media/<volume>/...). */
    public static String getVolumeName(Uri uri) {
        List<String> segments = uri.getPathSegments();
        if (!AUTHORITY.equals(uri.getAuthority()) || segments.isEmpty()) {
            throw new IllegalArgumentException("Missing volume name: " + uri);
        }
        return segments.get(0);
    }

    public static Set<String> getExternalVolumeNames(Context context) {
        return Collections.singleton(VOLUME_EXTERNAL_PRIMARY);
    }

    public static Set<String> getRecentExternalVolumeNames(Context context) {
        return getExternalVolumeNames(context);
    }

    public static String getVersion(Context context) {
        return "0";
    }

    public static String getVersion(Context context, String volumeName) {
        return "0";
    }

    public static long getGeneration(Context context, String volumeName) {
        return 0;
    }

    public static Uri getMediaScannerUri() {
        return AUTHORITY_URI.buildUpon().appendPath("none").appendPath("media_scanner").build();
    }

    /** Nothing scans: no URI for the file. */
    public static Uri scanFile(ContentResolver resolver, File file) {
        return null;
    }

    /** No trash without a media provider: the path is not moved (null, as for a failure). */
    public static String trashFile(ContentResolver resolver, String path) {
        return null;
    }

    public static String restoreFileFromTrash(ContentResolver resolver, String path, String volume) {
        return null;
    }

    static Uri contentUri(String volumeName, String... path) {
        Uri.Builder b = AUTHORITY_URI.buildUpon().appendPath(volumeName);
        for (String p : path) {
            b.appendPath(p);
        }
        return b.build();
    }

    public interface MediaColumns extends BaseColumns {
        String DATA = "_data";
        String SIZE = "_size";
        String DISPLAY_NAME = "_display_name";
        String TITLE = "title";
        String DATE_ADDED = "date_added";
        String DATE_MODIFIED = "date_modified";
        String DATE_TAKEN = "datetaken";
        String MIME_TYPE = "mime_type";
        String IS_PENDING = "is_pending";
        String IS_TRASHED = "is_trashed";
        String IS_FAVORITE = "is_favorite";
        String WIDTH = "width";
        String HEIGHT = "height";
        String ORIENTATION = "orientation";
        String OWNER_PACKAGE_NAME = "owner_package_name";
        String VOLUME_NAME = "volume_name";
        String RELATIVE_PATH = "relative_path";
        String BUCKET_ID = "bucket_id";
        String BUCKET_DISPLAY_NAME = "bucket_display_name";
        String DURATION = "duration";
        String ARTIST = "artist";
        String ALBUM = "album";
        String COMPOSER = "composer";
        String DOCUMENT_ID = "document_id";
    }

    public static final class Files {
        private Files() {
        }

        public static Uri getContentUri(String volumeName) {
            return contentUri(volumeName, "file");
        }

        public static Uri getContentUri(String volumeName, long rowId) {
            return contentUri(volumeName, "file", Long.toString(rowId));
        }

        public interface FileColumns extends MediaColumns {
            String MEDIA_TYPE = "media_type";
            int MEDIA_TYPE_NONE = 0;
            int MEDIA_TYPE_IMAGE = 1;
            int MEDIA_TYPE_AUDIO = 2;
            int MEDIA_TYPE_VIDEO = 3;
            int MEDIA_TYPE_PLAYLIST = 4;
            int MEDIA_TYPE_SUBTITLE = 5;
            int MEDIA_TYPE_DOCUMENT = 6;
            String PARENT = "parent";
        }
    }

    public static final class Audio {
        private Audio() {
        }

        public interface AudioColumns extends MediaColumns {
            String ARTIST_ID = "artist_id";
            String ALBUM_ID = "album_id";
            String TRACK = "track";
            String YEAR = "year";
            String IS_MUSIC = "is_music";
            String IS_PODCAST = "is_podcast";
            String IS_RINGTONE = "is_ringtone";
            String IS_ALARM = "is_alarm";
            String IS_NOTIFICATION = "is_notification";
            String IS_AUDIOBOOK = "is_audiobook";
        }

        public static final class Media implements AudioColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
            public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/audio";
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/audio";
            public static final String DEFAULT_SORT_ORDER = TITLE;
            public static final String RECORD_SOUND_ACTION = "android.provider.MediaStore.RECORD_SOUND";
            public static final String EXTRA_MAX_BYTES = "android.provider.MediaStore.extra.MAX_BYTES";

            private Media() {
            }

            public static Uri getContentUri(String volumeName) {
                return contentUri(volumeName, "audio", "media");
            }

            public static Uri getContentUri(String volumeName, long id) {
                return contentUri(volumeName, "audio", "media", Long.toString(id));
            }

            public static Uri getContentUriForPath(String path) {
                return EXTERNAL_CONTENT_URI;
            }
        }
    }

    public static final class Images {
        private Images() {
        }

        public interface ImageColumns extends MediaColumns {
            String DESCRIPTION = "description";
            String LATITUDE = "latitude";
            String LONGITUDE = "longitude";
        }

        public static final class Media implements ImageColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
            public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/image";
            public static final String DEFAULT_SORT_ORDER = BUCKET_DISPLAY_NAME;

            private Media() {
            }

            public static Uri getContentUri(String volumeName) {
                return contentUri(volumeName, "images", "media");
            }

            public static Uri getContentUri(String volumeName, long id) {
                return contentUri(volumeName, "images", "media", Long.toString(id));
            }
        }

        public static class Thumbnails implements BaseColumns {
            public static final int MINI_KIND = 1;
            public static final int FULL_SCREEN_KIND = 2;
            public static final int MICRO_KIND = 3;
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
            public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);

            public static Uri getContentUri(String volumeName) {
                return contentUri(volumeName, "images", "thumbnails");
            }

            /** The size MediaProvider gives each kind of thumbnail. */
            public static Size getKindSize(int kind) {
                switch (kind) {
                    case MINI_KIND:
                        return new Size(512, 384);
                    case FULL_SCREEN_KIND:
                        return new Size(1024, 786);
                    case MICRO_KIND:
                        return new Size(96, 96);
                    default:
                        throw new IllegalArgumentException("Unsupported kind: " + kind);
                }
            }
        }
    }

    public static final class Video {
        private Video() {
        }

        public interface VideoColumns extends MediaColumns {
            String RESOLUTION = "resolution";
            String DESCRIPTION = "description";
        }

        public static final class Media implements VideoColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
            public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/video";
            public static final String DEFAULT_SORT_ORDER = TITLE;

            private Media() {
            }

            public static Uri getContentUri(String volumeName) {
                return contentUri(volumeName, "video", "media");
            }

            public static Uri getContentUri(String volumeName, long id) {
                return contentUri(volumeName, "video", "media", Long.toString(id));
            }
        }
    }

    public static final class Downloads implements MediaColumns {
        public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
        public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);

        private Downloads() {
        }

        public static Uri getContentUri(String volumeName) {
            return contentUri(volumeName, "downloads");
        }
    }
}
