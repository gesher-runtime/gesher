/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Local provider-unavailable policy and adaptations: Copyright (C) 2026 Gesher Authors. */
package android.provider;

import android.app.PendingIntent;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.text.TextUtils;
import android.util.ArrayMap;
import android.util.Log;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.Collator;
import java.util.Locale;
import java.util.regex.Pattern;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Size;

import java.io.File;
import java.util.Collection;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;
import java.util.Set;

/**
 * MediaProvider module contracts (framework.jar compiles
 * against the module's API stubs and does not contain it). The framework calls it
 * where it touches media files: MediaPlayer.setDataSource(FileDescriptor, ...)
 * goes through FileUtils.convertToModernFd, which asks
 * getOriginalMediaFormatFileDescriptor for a transcoding-free descriptor (docs/media.md).
 * Gesher has no media provider: there is nothing to transcode, nothing is
 * scanned, and the content URIs name collections no provider answers. When a module jar
 * on the boot class path brings the real class, it wins.
 */
public final class MediaStore {
    private static final String TAG = "MediaStore";
    public static final String ACCESS_MEDIA_OWNER_PACKAGE_NAME_PERMISSION = "com.android.providers.media.permission.ACCESS_MEDIA_OWNER_PACKAGE_NAME";
    public static final String ACCESS_OEM_METADATA_PERMISSION = "com.android.providers.media.permission.ACCESS_OEM_METADATA";
    public static final String ACTION_MOTION_PHOTO_CAPTURE = "android.provider.action.MOTION_PHOTO_CAPTURE";
    public static final String ACTION_MOTION_PHOTO_CAPTURE_SECURE = "android.provider.action.MOTION_PHOTO_CAPTURE_SECURE";
    public static final String ACTION_PICK_IMAGES_SETTINGS = "android.provider.action.PICK_IMAGES_SETTINGS";
    public static final String ACTION_REVIEW = "android.provider.action.REVIEW";
    public static final String ACTION_REVIEW_SECURE = "android.provider.action.REVIEW_SECURE";
    public static final String EXTRA_ACCEPT_ORIGINAL_MEDIA_FORMAT = "android.provider.extra.ACCEPT_ORIGINAL_MEDIA_FORMAT";
    public static final String EXTRA_BRIGHTNESS = "android.provider.extra.BRIGHTNESS";
    public static final String EXTRA_FINISH_ON_COMPLETION = "android.intent.extra.finishOnCompletion";
    public static final String EXTRA_FULL_SCREEN = "android.intent.extra.fullScreen";
    public static final String EXTRA_MEDIA_ALBUM = "android.intent.extra.album";
    public static final String EXTRA_MEDIA_ARTIST = "android.intent.extra.artist";
    public static final String EXTRA_MEDIA_CAPABILITIES = "android.provider.extra.MEDIA_CAPABILITIES";
    public static final String EXTRA_MEDIA_CAPABILITIES_UID = "android.provider.extra.MEDIA_CAPABILITIES_UID";
    public static final String EXTRA_MEDIA_FOCUS = "android.intent.extra.focus";
    public static final String EXTRA_MEDIA_GENRE = "android.intent.extra.genre";
    public static final String EXTRA_MEDIA_PLAYLIST = "android.intent.extra.playlist";
    public static final String EXTRA_MEDIA_RADIO_CHANNEL = "android.intent.extra.radio_channel";
    public static final String EXTRA_MEDIA_TITLE = "android.intent.extra.title";
    public static final String EXTRA_PICKER_PRE_SELECTION_URIS = "android.provider.extra.PICKER_PRE_SELECTION_URIS";
    public static final String EXTRA_PICK_IMAGES_ACCENT_COLOR = "android.provider.extra.PICK_IMAGES_ACCENT_COLOR";
    public static final String EXTRA_PICK_IMAGES_IN_ORDER = "android.provider.extra.PICK_IMAGES_IN_ORDER";
    public static final String EXTRA_PICK_IMAGES_LAUNCH_TAB = "android.provider.extra.PICK_IMAGES_LAUNCH_TAB";
    public static final String EXTRA_SCREEN_ORIENTATION = "android.intent.extra.screenOrientation";
    public static final String EXTRA_SHOW_ACTION_ICONS = "android.intent.extra.showActionIcons";
    public static final String INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH = "android.media.action.MEDIA_PLAY_FROM_SEARCH";
    public static final String INTENT_ACTION_STILL_IMAGE_CAMERA_SECURE = "android.media.action.STILL_IMAGE_CAMERA_SECURE";
    public static final String INTENT_ACTION_TEXT_OPEN_FROM_SEARCH = "android.media.action.TEXT_OPEN_FROM_SEARCH";
    public static final String INTENT_ACTION_VIDEO_PLAY_FROM_SEARCH = "android.media.action.VIDEO_PLAY_FROM_SEARCH";
    public static final int MATCH_DEFAULT = 0;
    public static final int MATCH_EXCLUDE = 2;
    public static final int MATCH_INCLUDE = 1;
    public static final int MATCH_ONLY = 3;
    public static final String MEDIA_IGNORE_FILENAME = ".nomedia";
    public static final String MEDIA_SCANNER_VOLUME = "volume";
    public static final String META_DATA_REVIEW_GALLERY_PREWARM_SERVICE = "android.media.review_gallery_prewarm_service";
    public static final String META_DATA_STILL_IMAGE_CAMERA_PREWARM_SERVICE = "android.media.still_image_camera_preview_service";
    public static final int PICK_IMAGES_TAB_ALBUMS = 0;
    public static final int PICK_IMAGES_TAB_IMAGES = 1;
    public static final String QUERY_ARG_INCLUDE_RECENTLY_UNMOUNTED_VOLUMES = "android:query-arg-recently-unmounted-volumes";
    public static final String QUERY_ARG_LATEST_SELECTION_ONLY = "android:query-arg-latest-selection-only";
    public static final String QUERY_ARG_MATCH_FAVORITE = "android:query-arg-match-favorite";
    public static final String QUERY_ARG_MATCH_PENDING = "android:query-arg-match-pending";
    public static final String QUERY_ARG_MATCH_TRASHED = "android:query-arg-match-trashed";
    public static final String QUERY_ARG_MEDIA_STANDARD_SORT_ORDER = "android:query-arg-media-standard-sort-order";
    public static final String QUERY_ARG_RELATED_URI = "android:query-arg-related-uri";

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

    public MediaStore() {
    }

    /** Android 16's picker selection contract; this does not supply a picker activity. */
    public static int getPickImagesMaxLimit() {
        return 100;
    }

    @Deprecated
    public static Uri setIncludePending(Uri uri) {
        return uri.buildUpon().appendQueryParameter("includePending", "1").build();
    }

    public static Uri setRequireOriginal(Uri uri) {
        return uri.buildUpon().appendQueryParameter("requireOriginal", "1").build();
    }

    public static boolean getRequireOriginal(Uri uri) {
        return uri.getBooleanQueryParameter("requireOriginal", false);
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

    /* Android 16 MediaProvider call protocol. These methods only construct requests;
     * the resolver/provider owns authorization, approval and completion. In particular,
     * an absent provider retains ContentResolver's ordinary unknown-authority failure. */
    private static PendingIntent createRequest(ContentResolver resolver, String method,
            Collection<Uri> uris, ContentValues values) {
        Objects.requireNonNull(resolver);
        Objects.requireNonNull(uris);

        final Iterator<Uri> it = uris.iterator();
        final ClipData clipData = ClipData.newRawUri(null, it.next());
        while (it.hasNext()) {
            clipData.addItem(new ClipData.Item(it.next()));
        }

        final Bundle extras = new Bundle();
        extras.putParcelable("clip_data", clipData);
        extras.putParcelable("content_values", values);
        return resolver.call(AUTHORITY, method, null, extras).getParcelable("result");
    }

    public static PendingIntent createWriteRequest(ContentResolver resolver, Collection<Uri> uris) {
        return createRequest(resolver, "create_write_request", uris, null);
    }

    public static PendingIntent createDeleteRequest(ContentResolver resolver, Collection<Uri> uris) {
        return createRequest(resolver, "create_delete_request", uris, null);
    }

    public static PendingIntent createTrashRequest(ContentResolver resolver, Collection<Uri> uris,
            boolean value) {
        final ContentValues values = new ContentValues();
        values.put(MediaColumns.IS_TRASHED, value ? 1 : 0);
        return createRequest(resolver, "create_trash_request", uris, values);
    }

    public static PendingIntent createFavoriteRequest(ContentResolver resolver, Collection<Uri> uris,
            boolean value) {
        final ContentValues values = new ContentValues();
        values.put(MediaColumns.IS_FAVORITE, value ? 1 : 0);
        return createRequest(resolver, "create_favorite_request", uris, values);
    }

    public static void markIsFavoriteStatus(ContentResolver resolver, Collection<Uri> uris,
            boolean areFavorites) {
        Objects.requireNonNull(resolver);
        Objects.requireNonNull(uris);

        final Iterator<Uri> it = uris.iterator();
        final ClipData clipData = ClipData.newRawUri(null, it.next());
        while (it.hasNext()) {
            clipData.addItem(new ClipData.Item(it.next()));
        }

        final Bundle extras = new Bundle();
        extras.putParcelable("clip_data", clipData);
        extras.putBoolean(MediaColumns.IS_FAVORITE, areFavorites);
        resolver.call(AUTHORITY, "mark_media_as_favorite", null, extras);
    }

    public static boolean isCurrentSystemGallery(ContentResolver resolver, int uid,
            String packageName) {
        final Bundle in = new Bundle();
        in.putInt("is_system_gallery_uid", uid);
        final Bundle out = resolver.call(AUTHORITY, "is_system_gallery", packageName, in);
        return out.getBoolean("is_system_gallery_response");
    }

    public static boolean isCurrentCloudMediaProviderAuthority(ContentResolver resolver,
            String authority) {
        return callForCloudProvider(resolver, "is_current_cloud_provider", authority, null);
    }

    public static boolean isSupportedCloudMediaProviderAuthority(ContentResolver resolver,
            String authority) {
        return callForCloudProvider(resolver, "is_supported_cloud_provider", authority, null);
    }

    public static void notifyCloudMediaChangedEvent(ContentResolver resolver, String authority,
            String currentMediaCollectionId) {
        final Bundle extras = new Bundle();
        extras.putString("android.provider.extra.MEDIA_COLLECTION_ID", currentMediaCollectionId);
        if (!callForCloudProvider(resolver, "notify_cloud_media_changed_event", authority, extras)) {
            throw new SecurityException("Failed to notify cloud media changed event");
        }
    }

    private static boolean callForCloudProvider(ContentResolver resolver, String method,
            String callingAuthority, Bundle extras) {
        Objects.requireNonNull(resolver);
        Objects.requireNonNull(method);
        Objects.requireNonNull(callingAuthority);
        final Bundle out = resolver.call(AUTHORITY, method, callingAuthority, extras);
        return out.getBoolean("cloud_provider_result");
    }

    static Uri contentUri(String volumeName, String... path) {
        Uri.Builder b = AUTHORITY_URI.buildUpon().appendPath(volumeName);
        for (String p : path) {
            b.appendPath(p);
        }
        return b.build();
    }

    public interface MediaColumns extends BaseColumns {
        public static final String ALBUM_ARTIST = "album_artist";
        public static final String AUTHOR = "author";
        public static final String BITRATE = "bitrate";
        public static final String CAPTURE_FRAMERATE = "capture_framerate";
        public static final String CD_TRACK_NUMBER = "cd_track_number";
        public static final String COMPILATION = "compilation";
        public static final String DATE_EXPIRES = "date_expires";
        public static final String DISC_NUMBER = "disc_number";
        public static final String GENERATION_ADDED = "generation_added";
        public static final String GENERATION_MODIFIED = "generation_modified";
        public static final String GENRE = "genre";
        public static final String INFERRED_DATE = "inferred_date";
        public static final String INSTANCE_ID = "instance_id";
        public static final String IS_DOWNLOAD = "is_download";
        public static final String IS_DRM = "is_drm";
        public static final String NUM_TRACKS = "num_tracks";
        public static final String OEM_METADATA = "oem_metadata";
        public static final String ORIGINAL_DOCUMENT_ID = "original_document_id";
        public static final String RESOLUTION = "resolution";
        public static final String WRITER = "writer";
        public static final String XMP = "xmp";
        public static final String YEAR = "year";

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
        public Files() {
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
        public Audio() {
        }

        private static final Pattern PATTERN_TRIM_BEFORE = Pattern.compile(
                "(?i)(^(the|an|a) |,\\s*(the|an|a)$|[^\\w\\s]|^\\s+|\\s+$)");
        private static final Pattern PATTERN_TRIM_AFTER = Pattern.compile("(^(00)+|(00)+$)");

        /** AOSP's opaque, locale-independent legacy search key. */
        @Deprecated
        public static String keyFor(String name) {
            if (TextUtils.isEmpty(name)) return "";
            if (UNKNOWN_STRING.equals(name)) return "01";
            final boolean sortFirst = name.startsWith("\001");
            name = PATTERN_TRIM_BEFORE.matcher(name).replaceAll("");
            if (TextUtils.isEmpty(name)) return "";
            final Collator c = Collator.getInstance(Locale.ROOT);
            c.setStrength(Collator.PRIMARY);
            name = encodeToString(c.getCollationKey(name).toByteArray());
            name = PATTERN_TRIM_AFTER.matcher(name).replaceAll("");
            if (sortFirst) name = "01" + name;
            return name;
        }

        private static String encodeToString(byte[] bytes) {
            final StringBuilder sb = new StringBuilder();
            for (byte b : bytes) sb.append(String.format("%02x", b));
            return sb.toString();
        }

        public interface AudioColumns extends MediaColumns {
            public static final String ALBUM_KEY = "album_key";
            public static final String ARTIST_KEY = "artist_key";
            public static final String BITS_PER_SAMPLE = "bits_per_sample";
            public static final String BOOKMARK = "bookmark";
            public static final String GENRE = "genre";
            public static final String GENRE_ID = "genre_id";
            public static final String GENRE_KEY = "genre_key";
            public static final String IS_RECORDING = "is_recording";
            public static final String SAMPLERATE = "samplerate";
            public static final String TITLE_KEY = "title_key";
            public static final String TITLE_RESOURCE_URI = "title_resource_uri";

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

            public Media() {
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

        public interface AlbumColumns {
            public static final String ALBUM_ID = "album_id";
            public static final String ALBUM = "album";
            public static final String ARTIST_ID = "artist_id";
            public static final String ARTIST = "artist";
            public static final String ARTIST_KEY = "artist_key";
            public static final String NUMBER_OF_SONGS = "numsongs";
            public static final String NUMBER_OF_SONGS_FOR_ARTIST = "numsongs_by_artist";
            public static final String FIRST_YEAR = "minyear";
            public static final String LAST_YEAR = "maxyear";
            public static final String ALBUM_KEY = "album_key";
            public static final String ALBUM_ART = "album_art";
        }

        public interface ArtistColumns {
            public static final String ARTIST = "artist";
            public static final String ARTIST_KEY = "artist_key";
            public static final String NUMBER_OF_ALBUMS = "number_of_albums";
            public static final String NUMBER_OF_TRACKS = "number_of_tracks";
        }

        public interface GenresColumns {
            public static final String NAME = "name";
        }

        public interface PlaylistsColumns extends MediaColumns {
            public static final String NAME = "name";
            public static final String DATA = "_data";
            public static final String DATE_ADDED = "date_added";
            public static final String DATE_MODIFIED = "date_modified";
        }

        public static final class Albums implements BaseColumns, AlbumColumns {
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("audio")
                        .appendPath("albums").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/albums";
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/album";
            public static final String DEFAULT_SORT_ORDER = ALBUM_KEY;
        }

        public static final class Artists implements BaseColumns, ArtistColumns {
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("audio")
                        .appendPath("artists").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/artists";
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/artist";
            public static final String DEFAULT_SORT_ORDER = ARTIST_KEY;
            public static final class Albums implements BaseColumns, AlbumColumns {
                public static final Uri getContentUri(String volumeName,long artistId) {
                    return ContentUris
                            .withAppendedId(Audio.Artists.getContentUri(volumeName), artistId)
                            .buildUpon().appendPath("albums").build();
                }
            }
        }

        public static final class Genres implements BaseColumns, GenresColumns {
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("audio")
                        .appendPath("genres").build();
            }
            public static Uri getContentUriForAudioId(String volumeName, int audioId) {
                return ContentUris.withAppendedId(Audio.Media.getContentUri(volumeName), audioId)
                        .buildUpon().appendPath("genres").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/genre";
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/genre";
            public static final String DEFAULT_SORT_ORDER = NAME;
            public static final class Members implements AudioColumns {
                public static final Uri getContentUri(String volumeName, long genreId) {
                    return ContentUris
                            .withAppendedId(Audio.Genres.getContentUri(volumeName), genreId)
                            .buildUpon().appendPath("members").build();
                }
                public static final String CONTENT_DIRECTORY = "members";
                public static final String DEFAULT_SORT_ORDER = TITLE_KEY;
                public static final String AUDIO_ID = "audio_id";
                public static final String GENRE_ID = "genre_id";
            }
        }

        public static final class Playlists implements BaseColumns,
                PlaylistsColumns {
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("audio")
                        .appendPath("playlists").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/playlist";
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/playlist";
            public static final String DEFAULT_SORT_ORDER = NAME;
            public static final class Members implements AudioColumns {
                public static final Uri getContentUri(String volumeName, long playlistId) {
                    return ContentUris
                            .withAppendedId(Audio.Playlists.getContentUri(volumeName), playlistId)
                            .buildUpon().appendPath("members").build();
                }
                public static final boolean moveItem(ContentResolver res,
                        long playlistId, int from, int to) {
                    Uri uri = MediaStore.Audio.Playlists.Members.getContentUri("external",
                            playlistId)
                            .buildUpon()
                            .appendEncodedPath(String.valueOf(from))
                            .appendQueryParameter("move", "true")
                            .build();
                    ContentValues values = new ContentValues();
                    values.put(MediaStore.Audio.Playlists.Members.PLAY_ORDER, to);
                    return res.update(uri, values, null, null) != 0;
                }
                public static final String _ID = "_id";
                public static final String CONTENT_DIRECTORY = "members";
                public static final String AUDIO_ID = "audio_id";
                public static final String PLAYLIST_ID = "playlist_id";
                public static final String PLAY_ORDER = "play_order";
                public static final String DEFAULT_SORT_ORDER = PLAY_ORDER;
            }
        }

        public static final class Radio {
            public static final String ENTRY_CONTENT_TYPE = "vnd.android.cursor.item/radio";
            private Radio() { }
        }
    }

    public static final class Images {
        public Images() {
        }

        public interface ImageColumns extends MediaColumns {
            public static final String EXPOSURE_TIME = "exposure_time";
            public static final String F_NUMBER = "f_number";
            public static final String ISO = "iso";
            public static final String IS_PRIVATE = "isprivate";
            public static final String MINI_THUMB_MAGIC = "mini_thumb_magic";
            public static final String PICASA_ID = "picasa_id";
            public static final String SCENE_CAPTURE_TYPE = "scene_capture_type";

            String DESCRIPTION = "description";
            String LATITUDE = "latitude";
            String LONGITUDE = "longitude";
        }

        public static final class Media implements ImageColumns {
            public static final Cursor query(ContentResolver cr, Uri uri, String[] projection) {
                return cr.query(uri, projection, null, null, DEFAULT_SORT_ORDER);
            }
            public static final Cursor query(ContentResolver cr, Uri uri, String[] projection,
                    String where, String orderBy) {
                return cr.query(uri, projection, where,
                                             null, orderBy == null ? DEFAULT_SORT_ORDER : orderBy);
            }
            public static final Cursor query(ContentResolver cr, Uri uri, String[] projection,
                    String selection, String [] selectionArgs, String orderBy) {
                return cr.query(uri, projection, selection,
                        selectionArgs, orderBy == null ? DEFAULT_SORT_ORDER : orderBy);
            }
            public static final Bitmap getBitmap(ContentResolver cr, Uri url)
                    throws FileNotFoundException, IOException {
                try (InputStream input = cr.openInputStream(url)) {
                    return BitmapFactory.decodeStream(input);
                }
            }
            public static final String insertImage(ContentResolver cr, String imagePath,
                    String name, String description) throws FileNotFoundException {
                final Bitmap source;
                try {
                    source = ImageDecoder
                            .decodeBitmap(ImageDecoder.createSource(new File(imagePath)));
                } catch (IOException e) {
                    throw new FileNotFoundException(e.getMessage());
                }
                return insertImage(cr, source, name, description);
            }
            public static final String insertImage(ContentResolver cr, Bitmap source, String title,
                    String description) {
                if (TextUtils.isEmpty(title)) title = "Image";
                final long now = System.currentTimeMillis();
                final ContentValues values = new ContentValues();
                values.put(MediaColumns.DISPLAY_NAME, title);
                values.put(MediaColumns.MIME_TYPE, "image/jpeg");
                values.put(MediaColumns.DATE_ADDED, now / 1000);
                values.put(MediaColumns.DATE_MODIFIED, now / 1000);
                values.put(MediaColumns.IS_PENDING, 1);
                final Uri uri = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
                if (uri == null) return null;
                try {
                    try (OutputStream out = cr.openOutputStream(uri)) {
                        source.compress(Bitmap.CompressFormat.JPEG, 90, out);
                    }
                    values.clear();
                    values.put(MediaColumns.IS_PENDING, 0);
                    cr.update(uri, values, null, null);
                    return uri.toString();
                } catch (Exception e) {
                    Log.w(TAG, "Failed to insert image", e);
                    try {
                        cr.delete(uri, null, null);
                    } catch (Exception cleanupError) {
                        Log.w(TAG, "Failed to remove incomplete image", cleanupError);
                    }
                    return null;
                }
            }
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("images")
                        .appendPath("media").build();
            }
            public static Uri getContentUri(String volumeName, long id) {
                return ContentUris.withAppendedId(getContentUri(volumeName), id);
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/image";
            public static final String DEFAULT_SORT_ORDER = ImageColumns.BUCKET_DISPLAY_NAME;
        }

        public static class Thumbnails implements BaseColumns {
            public static final Cursor query(ContentResolver cr, Uri uri, String[] projection) {
                return cr.query(uri, projection, null, null, DEFAULT_SORT_ORDER);
            }
            public static final Cursor queryMiniThumbnails(ContentResolver cr, Uri uri, int kind,
                    String[] projection) {
                return cr.query(uri, projection, "kind = " + kind, null, DEFAULT_SORT_ORDER);
            }
            public static final Cursor queryMiniThumbnail(ContentResolver cr, long origId, int kind,
                    String[] projection) {
                return cr.query(EXTERNAL_CONTENT_URI, projection,
                        IMAGE_ID + " = " + origId + " AND " + KIND + " = " +
                        kind, null, null);
            }
            public static void cancelThumbnailRequest(ContentResolver cr, long origId) {
                final Uri uri = ContentUris.withAppendedId(
                        Images.Media.EXTERNAL_CONTENT_URI, origId);
                InternalThumbnails.cancelThumbnail(cr, uri);
            }
            public static Bitmap getThumbnail(ContentResolver cr, long imageId, int kind,
                    BitmapFactory.Options options) {
                final Uri uri = ContentUris.withAppendedId(
                        Images.Media.EXTERNAL_CONTENT_URI, imageId);
                return InternalThumbnails.getThumbnail(cr, uri, kind, options);
            }
            public static void cancelThumbnailRequest(ContentResolver cr, long origId,
                    long groupId) {
                cancelThumbnailRequest(cr, origId);
            }
            public static Bitmap getThumbnail(ContentResolver cr, long imageId, long groupId,
                    int kind, BitmapFactory.Options options) {
                return getThumbnail(cr, imageId, kind, options);
            }
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("images")
                        .appendPath("thumbnails").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String DEFAULT_SORT_ORDER = "image_id ASC";
            public static final String DATA = "_data";
            public static final String IMAGE_ID = "image_id";
            public static final String KIND = "kind";
            public static final int MINI_KIND = ThumbnailConstants.MINI_KIND;
            public static final int FULL_SCREEN_KIND = ThumbnailConstants.FULL_SCREEN_KIND;
            public static final int MICRO_KIND = ThumbnailConstants.MICRO_KIND;
            public static Size getKindSize(int kind) {
                return ThumbnailConstants.getKindSize(kind);
            }
            public static final String THUMB_DATA = "thumb_data";
            public static final String WIDTH = "width";
            public static final String HEIGHT = "height";
        }
    }

    public static final class Video {
        public static final String DEFAULT_SORT_ORDER = MediaColumns.DISPLAY_NAME;

        public Video() {
        }

        public static Cursor query(ContentResolver cr, Uri uri, String[] projection) {
            return cr.query(uri, projection, null, null, DEFAULT_SORT_ORDER);
        }

        public interface VideoColumns extends MediaColumns {
            public static final String BOOKMARK = "bookmark";
            public static final String CATEGORY = "category";
            public static final String COLOR_RANGE = "color_range";
            public static final String COLOR_STANDARD = "color_standard";
            public static final String COLOR_TRANSFER = "color_transfer";
            public static final String IS_PRIVATE = "isprivate";
            public static final String LANGUAGE = "language";
            public static final String LATITUDE = "latitude";
            public static final String LONGITUDE = "longitude";
            public static final String MINI_THUMB_MAGIC = "mini_thumb_magic";
            public static final String TAGS = "tags";

            String RESOLUTION = "resolution";
            String DESCRIPTION = "description";
        }

        public static final class Media implements VideoColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
            public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/video";
            public static final String DEFAULT_SORT_ORDER = TITLE;

            public Media() {
            }

            public static Uri getContentUri(String volumeName) {
                return contentUri(volumeName, "video", "media");
            }

            public static Uri getContentUri(String volumeName, long id) {
                return contentUri(volumeName, "video", "media", Long.toString(id));
            }
        }

        public static class Thumbnails implements BaseColumns {
            public static void cancelThumbnailRequest(ContentResolver cr, long origId) {
                final Uri uri = ContentUris.withAppendedId(
                        Video.Media.EXTERNAL_CONTENT_URI, origId);
                InternalThumbnails.cancelThumbnail(cr, uri);
            }
            public static Bitmap getThumbnail(ContentResolver cr, long videoId, int kind,
                    BitmapFactory.Options options) {
                final Uri uri = ContentUris.withAppendedId(
                        Video.Media.EXTERNAL_CONTENT_URI, videoId);
                return InternalThumbnails.getThumbnail(cr, uri, kind, options);
            }
            public static void cancelThumbnailRequest(ContentResolver cr, long videoId,
                    long groupId) {
                cancelThumbnailRequest(cr, videoId);
            }
            public static Bitmap getThumbnail(ContentResolver cr, long videoId, long groupId,
                    int kind, BitmapFactory.Options options) {
                return getThumbnail(cr, videoId, kind, options);
            }
            public static Uri getContentUri(String volumeName) {
                return AUTHORITY_URI.buildUpon().appendPath(volumeName).appendPath("video")
                        .appendPath("thumbnails").build();
            }
            public static final Uri INTERNAL_CONTENT_URI =
                    getContentUri("internal");
            public static final Uri EXTERNAL_CONTENT_URI =
                    getContentUri("external");
            public static final String DEFAULT_SORT_ORDER = "video_id ASC";
            public static final String DATA = "_data";
            public static final String VIDEO_ID = "video_id";
            public static final String KIND = "kind";
            public static final int MINI_KIND = ThumbnailConstants.MINI_KIND;
            public static final int FULL_SCREEN_KIND = ThumbnailConstants.FULL_SCREEN_KIND;
            public static final int MICRO_KIND = ThumbnailConstants.MICRO_KIND;
            public static Size getKindSize(int kind) {
                return ThumbnailConstants.getKindSize(kind);
            }
            public static final String WIDTH = "width";
            public static final String HEIGHT = "height";
        }
    }

    public static final class Downloads implements DownloadColumns {
        public static final String CONTENT_TYPE = "vnd.android.cursor.dir/download";

        public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL);
        public static final Uri EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);

        private Downloads() {
        }

        public static Uri getContentUri(String volumeName) {
            return contentUri(volumeName, "downloads");
        }

        public static Uri getContentUri(String volumeName, long id) {
            return ContentUris.withAppendedId(getContentUri(volumeName), id);
        }
    }

    public interface DownloadColumns extends MediaColumns {
        String DOWNLOAD_URI = "download_uri";
        String REFERER_URI = "referer_uri";
        String DESCRIPTION = "description";
    }

    public static class PickerMediaColumns {
        private PickerMediaColumns() {}
        public static final String DATA = MediaColumns.DATA;
        public static final String SIZE = MediaColumns.SIZE;
        public static final String DISPLAY_NAME = MediaColumns.DISPLAY_NAME;
        public static final String DATE_TAKEN = MediaColumns.DATE_TAKEN;
        public static final String MIME_TYPE = MediaColumns.MIME_TYPE;
        public static final String DURATION_MILLIS = MediaColumns.DURATION;
        public static final String WIDTH = "width";
        public static final String HEIGHT = "height";
        public static final String ORIENTATION = "orientation";
    }

    private static class InternalThumbnails implements BaseColumns {
        private static ArrayMap<Uri, CancellationSignal> sPending = new ArrayMap<>();
        static Bitmap getThumbnail(ContentResolver cr, Uri uri,
                int kind, BitmapFactory.Options opts) {
            final Size size = ThumbnailConstants.getKindSize(kind);
            CancellationSignal signal = null;
            synchronized (sPending) {
                signal = sPending.get(uri);
                if (signal == null) {
                    signal = new CancellationSignal();
                    sPending.put(uri, signal);
                }
            }
            try {
                return cr.loadThumbnail(uri, size, signal);
            } catch (IOException e) {
                Log.w(TAG, "Failed to obtain thumbnail for " + uri, e);
                return null;
            } finally {
                synchronized (sPending) {
                    sPending.remove(uri);
                }
            }
        }
        static void cancelThumbnail(ContentResolver cr, Uri uri) {
            synchronized (sPending) {
                final CancellationSignal signal = sPending.get(uri);
                if (signal != null) {
                    signal.cancel();
                }
            }
        }
    }

    private static class ThumbnailConstants {
        public static final int MINI_KIND = 1;
        public static final int FULL_SCREEN_KIND = 2;
        public static final int MICRO_KIND = 3;
        public static final Size MINI_SIZE = new Size(512, 384);
        public static final Size FULL_SCREEN_SIZE = new Size(1024, 786);
        public static final Size MICRO_SIZE = new Size(96, 96);
        public static Size getKindSize(int kind) {
            if (kind == ThumbnailConstants.MICRO_KIND) {
                return ThumbnailConstants.MICRO_SIZE;
            } else if (kind == ThumbnailConstants.FULL_SCREEN_KIND) {
                return ThumbnailConstants.FULL_SCREEN_SIZE;
            } else if (kind == ThumbnailConstants.MINI_KIND) {
                return ThumbnailConstants.MINI_SIZE;
            } else {
                throw new IllegalArgumentException("Unsupported kind: " + kind);
            }
        }
    }
}
