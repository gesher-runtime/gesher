/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Android-visible MediaStore contracts over the unchanged production MediaStoreIndex, a
 * real SQLite database file and a real shared-volume directory. Each check names the
 * app operation it models (ContentResolver insert/query/update/delete/openFileDescriptor,
 * MediaScannerConnection.scanFile, package removal, a system-server restart).
 */
public final class MediaStoreHost {
    private static int sChecks;

    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError("FAIL " + what);
        sChecks++;
    }

    private static void pass(String line) { System.out.println("PASS  " + line); }

    private static <T extends Throwable> T expect(Class<T> type, Runnable action, String what) {
        try {
            action.run();
        } catch (Throwable t) {
            if (type.isInstance(t)) return type.cast(t);
            throw new AssertionError("FAIL " + what + ": expected " + type.getSimpleName() + ", got " + t, t);
        }
        throw new AssertionError("FAIL " + what + ": expected " + type.getSimpleName());
    }

    // ---- fixtures ---------------------------------------------------------------------

    static final class Clock { long now = 1_790_000_000_000L; }

    static final class TestHost implements MediaStoreIndex.Host {
        final File root;
        final Clock clock;
        final List<String> notified = new ArrayList<>();
        TestHost(File root, Clock clock) { this.root = root; this.clock = clock; }
        @Override public File volumeRoot() { return root; }
        @Override public void notifyChange(Uri uri) { notified.add(uri.toString()); }
        @Override public long currentTimeMillis() { return clock.now; }
    }

    /** An installed app: its target SDK, granted permissions, app-op modes and grants. */
    static final class App implements MediaStoreIndex.Caller {
        final String pkg;
        final int uid;
        final int targetSdk;
        final Set<String> granted = new HashSet<>();
        final Map<String, Integer> ops = new HashMap<>();
        final Set<String> grants = new HashSet<>();
        final Set<String> visible = new HashSet<>();
        boolean requestedLegacy;
        boolean system;

        App(String pkg, int uid, int targetSdk, String... permissions) {
            this.pkg = pkg; this.uid = uid; this.targetSdk = targetSdk;
            granted.addAll(Arrays.asList(permissions));
            // AppOpsManager.opToDefaultMode: READ_MEDIA_* allowed, WRITE_MEDIA_* errored.
            for (String op : new String[] {MediaStoreIndex.OPSTR_READ_MEDIA_IMAGES,
                    MediaStoreIndex.OPSTR_READ_MEDIA_VIDEO, MediaStoreIndex.OPSTR_READ_MEDIA_AUDIO}) ops.put(op, 0);
            for (String op : new String[] {MediaStoreIndex.OPSTR_WRITE_MEDIA_IMAGES,
                    MediaStoreIndex.OPSTR_WRITE_MEDIA_VIDEO, MediaStoreIndex.OPSTR_WRITE_MEDIA_AUDIO}) ops.put(op, 2);
        }
        @Override public boolean self() { return system; }
        @Override public String packageName() { return pkg; }
        @Override public int uid() { return uid; }
        @Override public String[] sharedPackages() { return new String[] {pkg}; }
        @Override public int targetSdk() { return targetSdk; }
        @Override public boolean permission(String permission) { return system || granted.contains(permission); }
        @Override public int opMode(String op) { return ops.getOrDefault(op, 3); }
        @Override public boolean legacyStorageOp() {
            return !system && (targetSdk < 29 || (targetSdk < 30 && requestedLegacy));
        }
        @Override public boolean uriGranted(Uri uri, boolean write) { return grants.contains(uri.toString()); }
        @Override public boolean canSeePackage(String name) { return visible.contains(name); }
    }

    static byte[] png(int width, int height) {
        byte[] b = new byte[8 + 25 + 12];
        byte[] sig = {(byte) 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        System.arraycopy(sig, 0, b, 0, 8);
        b[11] = 13; b[12] = 'I'; b[13] = 'H'; b[14] = 'D'; b[15] = 'R';
        be32(b, 16, width); be32(b, 20, height);
        b[24] = 8; b[25] = 2;
        b[36] = 'I'; b[37] = 'E'; b[38] = 'N'; b[39] = 'D';
        return b;
    }

    /** A baseline JPEG header (SOF0) with an optional EXIF GPS IFD. */
    static byte[] jpeg(int width, int height, boolean gps) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        out.write(0xFF); out.write(0xD8);
        if (gps) {
            byte[] tiff = new byte[8 + 2 + 12 + 4 + 2 + 12 + 4];
            tiff[0] = 'M'; tiff[1] = 'M'; tiff[3] = 0x2A; be32(tiff, 4, 8);
            tiff[9] = 1;                                   // IFD0: one entry
            tiff[10] = (byte) 0x88; tiff[11] = 0x25;       // GPSInfo
            tiff[13] = 4; tiff[17] = 1; be32(tiff, 18, 26);
            tiff[27] = 1;                                  // GPS IFD: one entry
            tiff[29] = 1; tiff[31] = 2; tiff[35] = 2; tiff[36] = 'N';
            byte[] exif = new byte[6 + tiff.length];
            System.arraycopy("Exif\0\0".getBytes(), 0, exif, 0, 6);
            System.arraycopy(tiff, 0, exif, 6, tiff.length);
            out.write(0xFF); out.write(0xE1);
            out.write((exif.length + 2) >> 8); out.write((exif.length + 2) & 0xff);
            out.write(exif, 0, exif.length);
        }
        out.write(0xFF); out.write(0xC0); out.write(0); out.write(17); out.write(8);
        out.write(height >> 8); out.write(height & 0xff); out.write(width >> 8); out.write(width & 0xff);
        out.write(3);
        for (int i = 0; i < 9; i++) out.write(0);
        out.write(0xFF); out.write(0xD9);
        return out.toByteArray();
    }

    static void be32(byte[] b, int o, int v) {
        b[o] = (byte) (v >>> 24); b[o + 1] = (byte) (v >>> 16); b[o + 2] = (byte) (v >>> 8); b[o + 3] = (byte) v;
    }

    static void write(File file, byte[] data) {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(data);
        } catch (IOException e) {
            throw new AssertionError(e);
        }
    }

    static MediaStoreIndex.QueryArgs args() { return new MediaStoreIndex.QueryArgs(); }

    static MediaStoreIndex.QueryArgs args(String selection, String[] selectionArgs, String sort) {
        MediaStoreIndex.QueryArgs a = new MediaStoreIndex.QueryArgs();
        a.selection = selection; a.selectionArgs = selectionArgs; a.sortOrder = sort;
        return a;
    }

    static List<Map<String, String>> rows(Cursor c) {
        List<Map<String, String>> rows = new ArrayList<>();
        try (c) {
            String[] names = c.getColumnNames();
            while (c.moveToNext()) {
                Map<String, String> row = new HashMap<>();
                for (int i = 0; i < names.length; i++) row.put(names[i], c.isNull(i) ? null : c.getString(i));
                rows.add(row);
            }
        }
        return rows;
    }

    static final Uri IMAGES = Uri.parse("content://media/external_primary/images/media");
    static final Uri IMAGES_EXTERNAL = Uri.parse("content://media/external/images/media");
    static final Uri VIDEO = Uri.parse("content://media/external_primary/video/media");
    static final Uri AUDIO = Uri.parse("content://media/external_primary/audio/media");
    static final Uri DOWNLOADS = Uri.parse("content://media/external_primary/downloads");
    static final Uri FILES = Uri.parse("content://media/external/file");

    public static void main(String[] argv) throws Exception {
        File work = Files.createTempDirectory("gesher-mediastore-").toFile();
        File volume = new File(work, "data/media/0");
        volume.mkdirs();
        File dbFile = new File(work, "data/system/media/external.db");
        dbFile.getParentFile().mkdirs();
        Clock clock = new Clock();
        TestHost host = new TestHost(volume, clock);
        SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(dbFile, null);
        MediaStoreIndex index = new MediaStoreIndex(db, host);

        App camera = new App("com.example.camera", 10101, 35);           // no storage permissions
        App gallery = new App("com.example.gallery", 10102, 35, MediaStoreIndex.READ_MEDIA_IMAGES,
                MediaStoreIndex.READ_MEDIA_VIDEO);
        App stranger = new App("com.example.stranger", 10103, 35);
        App legacy = new App("com.example.legacy", 10104, 28, MediaStoreIndex.READ_EXTERNAL_STORAGE,
                MediaStoreIndex.WRITE_EXTERNAL_STORAGE);
        App oldReader = new App("com.example.oldreader", 10105, 32, MediaStoreIndex.READ_EXTERNAL_STORAGE);
        App music = new App("com.example.music", 10106, 35, MediaStoreIndex.READ_MEDIA_AUDIO);
        App system = new App("android", 1000, 36);
        system.system = true;

        // ---- 1. pending insert, write through the descriptor, publish ----------------
        ContentValues values = new ContentValues();
        values.put("_display_name", "sunset.png");
        values.put("mime_type", "image/png");
        values.put("relative_path", "Pictures/Gesher");
        values.put("is_pending", 1);
        values.put("datetaken", 1_789_999_000_000L);
        Uri photo = index.insert(camera, IMAGES, values);
        check(photo.toString().startsWith(IMAGES + "/"), "insert returns an item URI of the Images collection");
        long photoId = ContentUris.parseId(photo);
        Map<String, String> pending = rows(index.query(camera, photo, null, args())).get(0);
        String expires = Long.toString((clock.now + MediaStoreIndex.DEFAULT_DURATION_PENDING) / 1000);
        check(pending.get("_data").equals(new File(volume, "Pictures/Gesher/.pending-" + expires
                + "-sunset.png").getPath()), "pending file is .pending-<expires>-sunset.png: " + pending.get("_data"));
        check("1".equals(pending.get("is_pending")) && expires.equals(pending.get("date_expires")),
                "IS_PENDING=1 and DATE_EXPIRES=now+7d");
        check("com.example.camera".equals(pending.get("owner_package_name")), "owner is the inserting package");
        check("Pictures/Gesher/".equals(pending.get("relative_path")), "relative_path normalized to Pictures/Gesher/");
        check("external_primary".equals(pending.get("volume_name")), "volume_name external_primary");
        check("sunset.png".equals(pending.get("_display_name")), "display name without the pending prefix");
        check(new File(volume, "Pictures/Gesher").isDirectory() && !new File(pending.get("_data")).exists(),
                "insert creates the directory, not the file");
        check(rows(index.query(camera, IMAGES, null, args())).isEmpty(), "default query excludes pending items, even the owner's");
        MediaStoreIndex.QueryArgs includePending = args();
        includePending.matchPending = MediaStoreIndex.MATCH_INCLUDE;
        check(rows(index.query(camera, IMAGES, null, includePending)).size() == 1,
                "QUERY_ARG_MATCH_PENDING=MATCH_INCLUDE shows the owner's pending item");
        check(rows(index.query(gallery, IMAGES, null, includePending)).size() == 1,
                "a READ_MEDIA_IMAGES holder sees the pending item only when it asks for pending items");
        check(rows(index.query(camera, Uri.parse(IMAGES + "?includePending=1"), null, args())).size() == 1,
                "legacy includePending=1 parameter");
        IllegalStateException notOwner = expect(IllegalStateException.class, () -> {
            try { index.openFileTarget(gallery, photo, false); } catch (IOException e) { throw new RuntimeException(e); }
        }, "only the owner opens a pending item");
        check(notOwner.getMessage().contains("Only owner is able to interact with pending/trashed item"), "pending owner message");
        MediaStoreIndex.FileTarget target = index.openFileTarget(camera, photo, true);
        write(target.file, png(640, 480));
        index.fileWritten(target.id);
        ContentValues publish = new ContentValues();
        publish.put("is_pending", 0);
        check(index.update(camera, photo, publish, args()) == 1, "update IS_PENDING=0 publishes one row");
        Map<String, String> published = rows(index.query(camera, photo, null, args())).get(0);
        File finalFile = new File(volume, "Pictures/Gesher/sunset.png");
        check(published.get("_data").equals(finalFile.getPath()) && finalFile.isFile()
                && !target.file.exists(), "publishing renames the file to its display name");
        check("0".equals(published.get("is_pending")) && published.get("date_expires") == null, "published row is not pending");
        check("640".equals(published.get("width")) && "480".equals(published.get("height"))
                && Long.toString(finalFile.length()).equals(published.get("_size")),
                "width/height/_size come from the written file");
        check("1789999000000".equals(published.get("datetaken")), "app-supplied DATE_TAKEN kept");
        check(rows(index.query(camera, IMAGES_EXTERNAL, null, args())).size() == 1, "published item in external images");
        check(host.notified.contains(IMAGES.toString()) && host.notified.contains(IMAGES_EXTERNAL.toString()),
                "observers of the Images collection are notified");
        pass("media: insert(RELATIVE_PATH, IS_PENDING=1) -> openFileDescriptor(\"w\") -> update(IS_PENDING=0) "
                + "places Pictures/Gesher/sunset.png, publishes it, and indexes its real size and 640x480");

        // ---- 2. scoped-storage ownership and permissions --------------------------------
        check(rows(index.query(stranger, IMAGES, null, args())).isEmpty(), "an app without READ_MEDIA_IMAGES sees no foreign images");
        expect(SecurityException.class, () -> {
            try { index.openFileTarget(stranger, photo, false); } catch (IOException e) { throw new RuntimeException(e); }
        }, "an app without permission cannot open another app's image");
        check(rows(index.query(gallery, IMAGES, null, args())).size() == 1, "READ_MEDIA_IMAGES reads every image");
        check(index.openFileTarget(gallery, photo, false).file.equals(finalFile), "READ_MEDIA_IMAGES opens another app's image");
        check(rows(index.query(gallery, VIDEO, null, args())).isEmpty() && rows(index.query(stranger, AUDIO, null, args())).isEmpty(),
                "collections are separate");
        ContentValues rename = new ContentValues();
        rename.put("_display_name", "stolen.png");
        SecurityException denied = expect(SecurityException.class,
                () -> index.update(gallery, photo, rename, args()), "a reader cannot update another app's image");
        check(denied.getMessage().contains("has no access to"), "write denial message");
        expect(SecurityException.class, () -> index.delete(gallery, photo, args()), "a reader cannot delete another app's image");
        check(index.delete(gallery, IMAGES, args()) == 0 && finalFile.isFile(),
                "a collection delete by a reader removes nothing of others");
        check(rows(index.query(oldReader, IMAGES, null, args())).size() == 1,
                "targetSdk 32 reads images with READ_EXTERNAL_STORAGE");
        App oldWithoutPermission = new App("com.example.old2", 10107, 32, MediaStoreIndex.READ_MEDIA_IMAGES);
        check(rows(index.query(oldWithoutPermission, IMAGES, null, args())).isEmpty(),
                "targetSdk 32 needs READ_EXTERNAL_STORAGE, not READ_MEDIA_IMAGES");
        App opDenied = new App("com.example.opdenied", 10108, 35, MediaStoreIndex.READ_MEDIA_IMAGES);
        opDenied.ops.put(MediaStoreIndex.OPSTR_READ_MEDIA_IMAGES, 1);
        check(rows(index.query(opDenied, IMAGES, null, args())).isEmpty(), "READ_MEDIA_IMAGES app op ignored -> no access");
        App granted = new App("com.example.share", 10109, 35);
        granted.grants.add(photo.toString());
        check(rows(index.query(granted, photo, null, args())).size() == 1
                && rows(index.query(granted, IMAGES, null, args())).isEmpty(),
                "a URI grant opens that item, never the collection");
        Map<String, String> galleryView = rows(index.query(gallery, photo, new String[] {"owner_package_name"}, args())).get(0);
        check(galleryView.get("owner_package_name") == null, "API 34+: an invisible owner package reads as NULL");
        gallery.visible.add("com.example.camera");
        galleryView = rows(index.query(gallery, photo, new String[] {"owner_package_name"}, args())).get(0);
        check("com.example.camera".equals(galleryView.get("owner_package_name")), "a visible owner package is reported");
        check(index.update(legacy, photo, rename, args()) == 1, "a legacy writer (targetSdk 28 + WRITE_EXTERNAL_STORAGE) may rename");
        File renamed = new File(volume, "Pictures/Gesher/stolen.png");
        check(renamed.isFile() && !finalFile.exists(), "rename moves the file");
        rename.put("_display_name", "sunset.png");
        check(index.update(camera, photo, rename, args()) == 1 && finalFile.isFile(), "the owner renames it back");
        ContentValues owner = new ContentValues();
        owner.put("owner_package_name", "com.example.stranger");
        index.update(camera, photo, owner, args());
        check("com.example.camera".equals(rows(index.query(camera, photo, null, args())).get(0).get("owner_package_name")),
                "an app cannot transfer ownership");
        pass("media: an app sees and edits its own rows; READ_MEDIA_IMAGES (READ_EXTERNAL_STORAGE below API 33) "
                + "reads others' images; writing others' needs consent -> SecurityException; legacy writers keep access");

        // ---- 3. placement rules ------------------------------------------------------------
        ContentValues wrongDir = new ContentValues();
        wrongDir.put("_display_name", "a.jpg");
        wrongDir.put("relative_path", "Download/");
        IllegalArgumentException dir = expect(IllegalArgumentException.class,
                () -> index.insert(camera, IMAGES, wrongDir), "images outside DCIM/Pictures");
        check(dir.getMessage().startsWith("Primary directory Download not allowed for " + IMAGES
                + "; allowed directories are [DCIM, Pictures]"), dir.getMessage());
        ContentValues wrongType = new ContentValues();
        wrongType.put("_display_name", "clip.mp4");
        wrongType.put("mime_type", "video/mp4");
        IllegalArgumentException type = expect(IllegalArgumentException.class,
                () -> index.insert(camera, IMAGES, wrongType), "video MIME type into images");
        check(type.getMessage().contains("expected MIME type under image/*"), type.getMessage());
        ContentValues data = new ContentValues();
        data.put("_data", new File(volume, "Pictures/x.jpg").getPath());
        expect(IllegalArgumentException.class, () -> index.insert(camera, IMAGES, data),
                "API 34+: inserting _data is rejected");
        ContentValues dup = new ContentValues();
        dup.put("_display_name", "sunset.png");
        dup.put("relative_path", "Pictures/Gesher/");
        Uri dupUri = index.insert(camera, IMAGES, dup);
        check(rows(index.query(camera, dupUri, null, args())).get(0).get("_data")
                .equals(new File(volume, "Pictures/Gesher/sunset (1).png").getPath()), "unique name sunset (1).png");
        ContentValues noExt = new ContentValues();
        noExt.put("_display_name", "IMG_2026");
        noExt.put("mime_type", "image/jpeg");
        Uri noExtUri = index.insert(camera, IMAGES, noExt);
        Map<String, String> noExtRow = rows(index.query(camera, noExtUri, null, args())).get(0);
        check(noExtRow.get("_data").equals(new File(volume, "Pictures/IMG_2026.jpg").getPath())
                && "Pictures/".equals(noExtRow.get("relative_path")),
                "default Pictures/ and the MIME type's extension: " + noExtRow.get("_data"));
        expect(IllegalArgumentException.class, () -> index.query(camera,
                Uri.parse("content://media/internal/images/media"), null, args()), "internal volume is not attached");
        check(index.query(legacy, Uri.parse("content://media/internal/images/media"), null, args()) == null,
                "pre-Q callers get null for an unattached volume (translateForQuery)");
        pass("media: MediaProvider placement rules: DCIM/Pictures only for images, MIME family checks, "
                + "_data rejected for API 34+, unique names, default directories, internal volume unattached");

        // ---- 4. Downloads, Audio, Files ----------------------------------------------
        ContentValues download = new ContentValues();
        download.put("_display_name", "report.pdf");
        download.put("mime_type", "application/pdf");
        Uri report = index.insert(camera, DOWNLOADS, download);
        check(report.toString().startsWith(DOWNLOADS + "/"), "Downloads insert URI");
        Map<String, String> reportRow = rows(index.query(camera, report, null, args())).get(0);
        check("Download/".equals(reportRow.get("relative_path")) && "1".equals(reportRow.get("is_download")),
                "Downloads row is under Download/ and is_download=1");
        MediaStoreIndex.FileTarget pdf = index.openFileTarget(camera, report, true);
        write(pdf.file, "%PDF-1.7\n".getBytes());
        index.fileWritten(pdf.id);
        check(rows(index.query(stranger, DOWNLOADS, null, args())).isEmpty(), "others' downloads are invisible");
        check(rows(index.query(legacy, DOWNLOADS, null, args())).size() == 1, "legacy readers see all downloads");
        ContentValues song = new ContentValues();
        song.put("_display_name", "theme.mp3");
        song.put("relative_path", "Music/");
        Uri songUri = index.insert(camera, AUDIO, song);
        check(rows(index.query(music, AUDIO, null, args())).size() == 1 && rows(index.query(stranger, AUDIO, null, args())).isEmpty(),
                "READ_MEDIA_AUDIO reads audio");
        Map<String, String> songRow = rows(index.query(camera, songUri, null, args())).get(0);
        check("audio/mpeg".equals(songRow.get("mime_type")) && "2".equals(
                rows(index.query(camera, Uri.parse(FILES + "/" + ContentUris.parseId(songUri)),
                        new String[] {"media_type"}, args())).get(0).get("media_type")), "audio media_type 2");
        check(rows(index.query(camera, FILES, null, args())).size() == 5, "Files shows the owner's every row");
        check(rows(index.query(gallery, FILES, null, args())).size() == 3, "Files shows a reader the images it may read");
        pass("media: Downloads (Download/, is_download), Audio (READ_MEDIA_AUDIO) and Files collections over one files table");

        // ---- 5. query: selection, sort, strict mode ---------------------------------------
        for (String name : new String[] {"b.png", "c.png", "a.png"}) {
            ContentValues v = new ContentValues();
            v.put("_display_name", name);
            v.put("relative_path", "DCIM/Camera/");
            index.insert(camera, IMAGES, v);
        }
        List<Map<String, String>> sorted = rows(index.query(camera, IMAGES, new String[] {"_id", "_display_name"},
                args("relative_path=?", new String[] {"DCIM/Camera/"}, "_display_name DESC")));
        check(sorted.size() == 3 && "c.png".equals(sorted.get(0).get("_display_name"))
                && "a.png".equals(sorted.get(2).get("_display_name")), "selection with args and sort order");
        MediaStoreIndex.QueryArgs limited = args(null, null, "_display_name ASC");
        limited.limit = "1,2";
        List<Map<String, String>> page = rows(index.query(camera, IMAGES, new String[] {"_display_name"}, limited));
        check(page.size() == 2 && "a.png".equals(page.get(0).get("_display_name"))
                && "b.png".equals(page.get(1).get("_display_name")), "QUERY_ARG_SQL_LIMIT offset,limit: " + page);
        // SQLiteQueryBuilder.query compiles the unwrapped statement first: a malformed
        // selection fails (android.database.sqlite.SQLiteException, parceled as such).
        expect(android.database.sqlite.SQLiteException.class, () -> index.query(camera, IMAGES,
                new String[] {"_id"}, args("_id = = 1", null, null)), "malformed selection");
        expect(IllegalArgumentException.class, () -> index.query(camera, IMAGES, new String[] {"_id"},
                args(null, null, "(SELECT owner_package_name FROM files)")), "strict sort order");
        expect(IllegalArgumentException.class, () -> index.query(camera, IMAGES, new String[] {"sqlite_version()"},
                args()), "strict projection");
        expect(IllegalArgumentException.class, () -> index.query(camera, IMAGES, new String[] {"_id"},
                args("_id IN (SELECT _id FROM files)", null, null)), "strict grammar rejects subqueries");
        // An attempt to close the access clause stays wrapped in its own parentheses.
        check(rows(index.query(stranger, IMAGES, new String[] {"_id"},
                args("1=1) OR (1=1", null, null))).isEmpty(), "a selection cannot escape the owner filter");
        check(rows(index.query(stranger, IMAGES, new String[] {"_id"}, args("1=1", null, null))).isEmpty(),
                "a selection cannot widen access");
        pass("media: query selection/args, sort and limit through AOSP SQLiteQueryBuilder strict mode; "
                + "unknown columns/tokens -> IllegalArgumentException, malformed SQL -> SQLiteException, "
                + "no selection widens access");

        // ---- 6. delete ----------------------------------------------------------------------
        File dupFile = new File(rows(index.query(camera, dupUri, null, args())).get(0).get("_data"));
        write(dupFile, png(1, 1));
        check(index.delete(camera, dupUri, args()) == 1 && !dupFile.exists(), "owner delete removes row and file");
        check(rows(index.query(camera, dupUri, null, args())).isEmpty(), "deleted row is gone");
        check(index.delete(legacy, Uri.parse(IMAGES + "/" + ContentUris.parseId(noExtUri)), args()) == 1,
                "a legacy writer deletes another app's image");
        pass("media: delete(uri) removes the owner's row and file; others' rows need write access");

        // ---- 7. scanning files written directly ------------------------------------------
        File dcim = new File(volume, "DCIM/Camera");
        dcim.mkdirs();
        File shot = new File(dcim, "IMG_20261123_101500.jpg");
        write(shot, jpeg(1024, 768, false));
        Uri scanned = index.scanFile(shot);
        check(scanned != null && scanned.toString().startsWith(IMAGES + "/"), "scanFile answers an Images URI: " + scanned);
        Map<String, String> shotRow = rows(index.query(system, scanned, null, args())).get(0);
        check(shotRow.get("owner_package_name") == null && "1024".equals(shotRow.get("width"))
                && "768".equals(shotRow.get("height")) && "image/jpeg".equals(shotRow.get("mime_type"))
                && "DCIM/Camera/".equals(shotRow.get("relative_path"))
                && "Camera".equals(shotRow.get("bucket_display_name")), "scanned metadata from the file");
        check(rows(index.query(gallery, scanned, null, args())).size() == 1
                && rows(index.query(stranger, scanned, null, args())).isEmpty(), "scanned media has no owner");
        check(scanned.equals(index.scanFile(shot)) && rows(index.query(system, IMAGES,
                null, args("_data=?", new String[] {shot.getPath()}, null))).size() == 1, "rescanning updates in place");
        File hidden = new File(volume, "Pictures/.thumbnails/t.jpg");
        hidden.getParentFile().mkdirs();
        write(hidden, jpeg(8, 8, false));
        Uri hiddenUri = index.scanFile(hidden);
        check(hiddenUri != null && hiddenUri.toString().startsWith(FILES.toString().replace("external", "external_primary")),
                "hidden directories index as MEDIA_TYPE_NONE: " + hiddenUri);
        File nomediaDir = new File(volume, "Pictures/Private");
        nomediaDir.mkdirs();
        write(new File(nomediaDir, ".nomedia"), new byte[0]);
        write(new File(nomediaDir, "p.jpg"), jpeg(8, 8, false));
        check(!index.scanFile(new File(nomediaDir, "p.jpg")).toString().contains("/images/"), ".nomedia hides a directory");
        File outside = new File(work, "outside.jpg");
        write(outside, jpeg(8, 8, false));
        check(index.scanFile(outside) == null, "files outside the shared volume are not indexed");
        check(index.scanFile(new File(volume, "Music/missing.mp3")) == null, "a missing file scans to null");
        check(shot.delete() && index.scanFile(shot) == null
                && rows(index.query(system, scanned, null, args())).isEmpty(), "scanning a deleted file forgets it");
        pass("media: MediaScannerConnection.scanFile / ACTION_MEDIA_SCANNER_SCAN_FILE indexes a file written to "
                + "the shared volume with its real metadata, no owner, hidden/.nomedia rules, and forgets deleted files");

        // ---- 8. location metadata -------------------------------------------------------------
        File geo = new File(dcim, "IMG_20261123_120000.jpg");
        write(geo, jpeg(16, 16, true));
        Uri geoUri = index.scanFile(geo);
        UnsupportedOperationException redaction = expect(UnsupportedOperationException.class, () -> {
            try { index.openFileTarget(gallery, geoUri, false); } catch (IOException e) { throw new RuntimeException(e); }
        }, "location metadata is not handed to a reader without ACCESS_MEDIA_LOCATION");
        check(redaction.getMessage().contains("ACCESS_MEDIA_LOCATION"), redaction.getMessage());
        gallery.granted.add(MediaStoreIndex.ACCESS_MEDIA_LOCATION);
        check(index.openFileTarget(gallery, geoUri, false).file.equals(geo), "ACCESS_MEDIA_LOCATION opens the original");
        gallery.granted.remove(MediaStoreIndex.ACCESS_MEDIA_LOCATION);
        check(!MediaStoreIndex.LocationMetadata.mayContain(finalFile), "a PNG without metadata has no location");
        pass("media: a file with EXIF GPS is not opened unredacted for a non-owner without ACCESS_MEDIA_LOCATION "
                + "(Gesher has no redactor); location-free files open normally");

        // ---- 9. create*Request: validated, no consent screen ----------------------------------
        MediaStoreIndex.validateCreateRequest("create_write_request", List.of(photo), null);
        expect(IllegalArgumentException.class, () -> MediaStoreIndex.validateCreateRequest(
                "create_delete_request", List.of(IMAGES), null), "collection URIs are invalid");
        Set<String> badKeys = new HashSet<>(List.of("is_trashed"));
        expect(IllegalArgumentException.class, () -> MediaStoreIndex.validateCreateRequest(
                "create_favorite_request", List.of(photo), badKeys), "only is_favorite for favorite requests");
        pass("media: createWriteRequest/createDeleteRequest arguments are validated as AOSP does; the provider "
                + "then reports the missing consent screen instead of returning a PendingIntent");

        // ---- 10. version and generation ---------------------------------------------------
        long generation = index.generation("external_primary");
        ContentValues more = new ContentValues();
        more.put("_display_name", "gen.png");
        index.insert(camera, IMAGES, more);
        check(index.generation("external_primary") > generation, "generation increases on insert");
        String version = index.version(camera, "external_primary");
        App baklava = new App("com.example.b", 10110, 36);
        String locked = index.version(baklava, "external_primary");
        check(version.startsWith(MediaStoreIndex.DB_VERSION + ":") && !locked.contains(":")
                && !version.equals(locked), "getVersion: version:uuid, opaque per UID for API 36");
        expect(IllegalArgumentException.class, () -> index.generation("internal"), "unknown volume generation");
        pass("media: MediaStore.getVersion / getGeneration from the index (generation grows with every change)");

        // ---- 11. restart: the index persists in SQLite -----------------------------------
        long countBefore = rows(index.query(system, FILES, new String[] {"_id"}, args())).size();
        long generationBefore = index.generation("external_primary");
        db.close();
        SQLiteDatabase reopened = SQLiteDatabase.openOrCreateDatabase(dbFile, null);
        MediaStoreIndex restarted = new MediaStoreIndex(reopened, host);
        check(rows(restarted.query(system, FILES, new String[] {"_id"}, args())).size() == countBefore,
                "rows survive a restart");
        check(restarted.generation("external_primary") == generationBefore
                && restarted.version(camera, "external_primary").equals(version), "version and generation survive");
        check(rows(restarted.query(camera, photo, null, args())).size() == 1
                && restarted.openFileTarget(camera, photo, false).file.equals(finalFile), "the owner still owns its image");
        pass("media: the index (rows, owners, generation, version) persists across a system-server restart");

        // ---- 12. package removal and pending expiry ---------------------------------------
        ContentValues stale = new ContentValues();
        stale.put("_display_name", "draft.png");
        stale.put("is_pending", 1);
        Uri draft = restarted.insert(camera, IMAGES, stale);
        File draftFile = new File(rows(restarted.query(camera, draft, null, args())).get(0).get("_data"));
        write(draftFile, png(2, 2));
        File ownMedia = new File(volume, "Android/media/com.example.camera");
        ContentValues own = new ContentValues();
        own.put("_display_name", "cache.png");
        own.put("relative_path", "Android/media/com.example.camera/");
        Uri ownUri = restarted.insert(camera, IMAGES, own);
        check(ownMedia.isDirectory(), "an app may place items in its Android/media directory");
        restarted.packageOrphaned("com.example.camera");
        check(rows(restarted.query(camera, photo, null, args())).isEmpty()
                && rows(restarted.query(gallery, photo, null, args())).size() == 1
                && rows(restarted.query(system, photo, null, args())).get(0).get("owner_package_name") == null,
                "a removed package's published media stays, without an owner");
        check(rows(restarted.query(system, ownUri, null, args())).isEmpty(),
                "Android/media rows whose file is gone are deleted");
        check(restarted.deleteExpired() == 0 && draftFile.exists(), "unexpired pending items stay");
        clock.now += 8L * 24 * 3600 * 1000;
        check(restarted.deleteExpired() == 1 && !draftFile.exists()
                && rows(restarted.query(system, draft, null, args())).isEmpty(),
                "an orphaned pending item is deleted with its file once DATE_EXPIRES passed");
        pass("media: package removal orphans published media and drops missing Android/media rows; "
                + "expired pending items (and their files) are deleted");

        reopened.close();
        System.out.println("media: " + sChecks + " checks passed");
    }
}
