/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.DownloadManager;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.provider.Downloads;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.Proxy;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Host contracts for the production downloads provider, queue and transfer. The app
 * side builds its ContentValues and selections exactly as android.app.DownloadManager
 * (android-16.0.0_r4) does; HTTP runs over real sockets to a local server; SQL runs in a
 * real SQLite engine through AOSP's SQLiteQueryBuilder.
 */
public final class DownloadManagerHost {
    static final int APP_UID = 10050, OTHER_UID = 10051;
    static final String APP = "com.example.app", OTHER = "com.example.other";
    static int failures;

    static void check(boolean condition, String message) {
        if (condition) {
            System.out.println("PASS " + message);
        } else {
            failures++;
            System.out.println("FAIL " + message);
        }
    }

    // ---- a local HTTP/1.1 server with explicit control of every byte ------------------

    static final class Request {
        String method, path;
        final Map<String, String> headers = new HashMap<>();
        String header(String name) { return headers.get(name.toLowerCase(Locale.ROOT)); }
    }

    interface Handler { void handle(Request request, OutputStream out) throws IOException; }

    static final class Server implements AutoCloseable {
        final ServerSocket socket;
        final Map<String, Handler> handlers = new HashMap<>();
        final List<Request> requests = Collections.synchronizedList(new ArrayList<>());
        Server() throws IOException {
            socket = new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
            Thread thread = new Thread(this::serve, "download-test-server");
            thread.setDaemon(true);
            thread.start();
        }
        String url(String path) { return "http://127.0.0.1:" + socket.getLocalPort() + path; }
        List<Request> requests(String path) {
            List<Request> result = new ArrayList<>();
            synchronized (requests) { for (Request r : requests) if (r.path.equals(path)) result.add(r); }
            return result;
        }
        private void serve() {
            while (!socket.isClosed()) {
                try {
                    Socket client = socket.accept();
                    Thread t = new Thread(() -> handle(client));
                    t.setDaemon(true);
                    t.start();
                } catch (IOException closed) {
                    return;
                }
            }
        }
        private void handle(Socket client) {
            try (Socket s = client) {
                InputStream in = s.getInputStream();
                Request request = new Request();
                String line = readLine(in);
                if (line == null) return;
                String[] parts = line.split(" ");
                request.method = parts[0];
                request.path = parts[1];
                while ((line = readLine(in)) != null && !line.isEmpty()) {
                    int colon = line.indexOf(':');
                    request.headers.put(line.substring(0, colon).trim().toLowerCase(Locale.ROOT),
                            line.substring(colon + 1).trim());
                }
                requests.add(request);
                Handler handler = handlers.get(request.path);
                OutputStream out = s.getOutputStream();
                if (handler == null) {
                    out.write(("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                            .getBytes(StandardCharsets.US_ASCII));
                } else {
                    handler.handle(request, out);
                }
                out.flush();
            } catch (IOException ignored) {
                // The client went away.
            }
        }
        private static String readLine(InputStream in) throws IOException {
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            int c;
            while ((c = in.read()) != -1) {
                if (c == '\n') break;
                if (c != '\r') line.write(c);
            }
            if (c == -1 && line.size() == 0) return null;
            return line.toString(StandardCharsets.ISO_8859_1);
        }
        @Override public void close() throws IOException { socket.close(); }
    }

    static void respond(OutputStream out, String status, String headers, byte[] body) throws IOException {
        out.write(("HTTP/1.1 " + status + "\r\n" + headers + "\r\n").getBytes(StandardCharsets.US_ASCII));
        if (body != null) out.write(body);
    }

    static byte[] payload(int size, int seed) {
        byte[] data = new byte[size];
        for (int i = 0; i < size; i++) data[i] = (byte) ((i * 31 + seed) & 0xff);
        return data;
    }

    // ---- the system around the provider ------------------------------------------------

    static final class Clock { final AtomicLong offset = new AtomicLong(); long now() { return System.currentTimeMillis() + offset.get(); } }

    static final class Broadcast {
        final String packageName, className; final boolean publicApi; final long id;
        Broadcast(String p, String c, boolean pub, long id) { packageName = p; className = c; publicApi = pub; this.id = id; }
    }

    static final class Fixture implements AutoCloseable {
        final File root, external, files, cache, dbFile;
        final Clock clock;
        final List<Broadcast> broadcasts = Collections.synchronizedList(new ArrayList<>());
        final List<String> grants = Collections.synchronizedList(new ArrayList<>());
        final List<String> revokes = Collections.synchronizedList(new ArrayList<>());
        final AtomicInteger changes = new AtomicInteger();
        volatile DownloadQueue.NetworkState network = new DownloadQueue.NetworkState(false, false);
        volatile boolean cleartext = true;
        /**
         * WindowsHost.powerStatus(): {ACLineStatus, BatteryFlag, BatteryLifePercent,
         * SystemStatusFlag}. A desktop without a battery by default: on AC, flag 128.
         */
        volatile int[] power = {1, 128, 255, 0};
        /** When the session display went off (test clock); -1 while it is on. */
        volatile long displayOffSince = -1;
        volatile Runnable conditionsListener;
        SQLiteDatabase db;
        DownloadRequests requests;
        DownloadQueue queue;

        Fixture(File root, Clock clock) {
            this.root = root;
            this.clock = clock;
            external = new File(root, "media/0");
            files = new File(root, "system/downloads/files");
            cache = new File(root, "system/downloads/cache");
            dbFile = new File(root, "system/downloads/downloads.db");
            external.mkdirs(); files.mkdirs(); cache.mkdirs();
            open();
        }

        void open() {
            db = SQLiteDatabase.openOrCreateDatabase(dbFile, null);
            DownloadQueue[] holder = new DownloadQueue[1];
            requests = new DownloadRequests(db, new DownloadRequests.Host() {
                @Override public File externalRoot() { return external; }
                @Override public File filesDir() { return files; }
                @Override public File cacheDir() { return cache; }
                @Override public boolean uidOwnsPackage(int uid, String pkg) {
                    return (uid == APP_UID && APP.equals(pkg)) || (uid == OTHER_UID && OTHER.equals(pkg));
                }
                @Override public void grantAllDownloads(String pkg, long id) { grants.add(pkg + ":" + id); }
                @Override public void revokeAllDownloads(long id) { revokes.add(Long.toString(id)); }
                @Override public void notifyChange(Uri uri) { changes.incrementAndGet(); }
                @Override public void sendCompleted(String pkg, String cls, boolean pub, long id, String extras) {
                    broadcasts.add(new Broadcast(pkg, cls, pub, id));
                }
                @Override public void schedule(long id) { holder[0].schedule(id); }
                @Override public void cancel(long id) { holder[0].cancel(id); }
                @Override public long currentTimeMillis() { return clock.now(); }
            });
            queue = new DownloadQueue(requests, new DownloadQueue.Environment() {
                @Override public DownloadQueue.NetworkState network() { return network; }
                @Override public long maxBytesOverMobile() { return Long.MAX_VALUE; }
                @Override public long recommendedMaxBytesOverMobile() { return Long.MAX_VALUE; }
                @Override public DownloadQueue.Transport transport(DownloadRequests.Info info) {
                    return new DownloadQueue.Transport() {
                        @Override public boolean cleartextPermitted(String host) { return cleartext; }
                        @Override public javax.net.ssl.SSLSocketFactory socketFactory() {
                            return javax.net.ssl.HttpsURLConnection.getDefaultSSLSocketFactory();
                        }
                        @Override public HttpURLConnection open(URL url) throws IOException {
                            return (HttpURLConnection) url.openConnection(Proxy.NO_PROXY);
                        }
                    };
                }
                @Override public String userAgent() { return "AndroidDownloadManager/16 (Linux; U; Android 16)"; }
                @Override public long currentTimeMillis() { return clock.now(); }
                @Override public long elapsedRealtime() { return System.nanoTime() / 1_000_000; }
                // The device conditions JobScheduler's charging and idle constraints read.
                public int[] powerStatus() { return power; }
                /** WindowsHost.jobIdleMillis: display-off time without input, 0 while on. */
                public long idleMillis() {
                    long since = displayOffSince;
                    return since < 0 ? 0 : Math.max(0, clock.now() - since);
                }
                public void listen(Runnable listener) { conditionsListener = listener; }
            });
            holder[0] = queue;
        }

        void restart() {
            queue.shutdown();
            db.close();
            open();
            queue.start();
        }

        @Override public void close() { queue.shutdown(); db.close(); }

        Cursor dmQuery(DownloadRequests.Caller caller, long... ids) {
            StringBuilder where = new StringBuilder("(");
            String[] args = new String[ids.length];
            for (int i = 0; i < ids.length; i++) {
                if (i > 0) where.append("OR ");
                where.append("_id = ? ");
                args[i] = Long.toString(ids[i]);
            }
            where.append(") AND deleted != '1'");
            return requests.query(caller, Downloads.Impl.CONTENT_URI, DownloadManager.UNDERLYING_COLUMNS,
                    where.toString(), args, "lastmod DESC");
        }

        Map<String, String> row(DownloadRequests.Caller caller, long id) {
            try (Cursor c = dmQuery(caller, id)) {
                if (!c.moveToFirst()) return null;
                Map<String, String> row = new HashMap<>();
                for (String column : c.getColumnNames()) row.put(column, c.getString(c.getColumnIndexOrThrow(column)));
                return row;
            }
        }

        int status(long id) {
            DownloadRequests.Info info = requests.info(id);
            return info == null ? -1 : info.status;
        }

        void await(long id, int... statuses) throws InterruptedException {
            long deadline = System.currentTimeMillis() + 20_000;
            while (System.currentTimeMillis() < deadline) {
                int status = status(id);
                for (int s : statuses) if (s == status) { queue.awaitIdle(5_000); return; }
                Thread.sleep(10);
            }
            queue.awaitIdle(5_000);
        }

        /** Windows reported a power status or session display change. */
        boolean conditionsChanged() {
            Runnable listener = conditionsListener;
            if (listener == null) return false;
            listener.run();
            return true;
        }

        List<Broadcast> broadcastsFor(long id) {
            List<Broadcast> result = new ArrayList<>();
            synchronized (broadcasts) { for (Broadcast b : broadcasts) if (b.id == id) result.add(b); }
            return result;
        }
    }

    static DownloadRequests.Caller caller(int uid, String pkg, String... permissions) {
        Set<String> held = new HashSet<>(Arrays.asList(permissions));
        return new DownloadRequests.Caller() {
            @Override public int uid() { return uid; }
            @Override public String packageName() { return pkg; }
            @Override public boolean holds(String permission) { return held.contains(permission); }
            @Override public boolean local() { return false; }
            @Override public boolean legacyStorage() { return false; }
            @Override public boolean writeExternalStorage() { return false; }
            @Override public boolean installer() { return false; }
        };
    }

    /** DownloadManager.Request.toContentValues with the Request defaults. */
    static ContentValues request(String url, String destinationUri, String... headers) {
        ContentValues values = new ContentValues();
        values.put(Downloads.Impl.COLUMN_URI, url);
        values.put(Downloads.Impl.COLUMN_IS_PUBLIC_API, true);
        values.put(Downloads.Impl.COLUMN_NOTIFICATION_PACKAGE, APP);
        if (destinationUri != null) {
            values.put(Downloads.Impl.COLUMN_DESTINATION, Downloads.Impl.DESTINATION_FILE_URI);
            values.put(Downloads.Impl.COLUMN_FILE_NAME_HINT, destinationUri);
        } else {
            values.put(Downloads.Impl.COLUMN_DESTINATION, Downloads.Impl.DESTINATION_CACHE_PARTITION_PURGEABLE);
        }
        values.put(Downloads.Impl.COLUMN_MEDIA_SCANNED, Downloads.Impl.MEDIA_NOT_SCANNED);
        for (int i = 0; i < headers.length; i++) {
            values.put(Downloads.Impl.RequestHeaders.INSERT_KEY_PREFIX + i, headers[i]);
        }
        values.put(Downloads.Impl.COLUMN_VISIBILITY, DownloadManager.Request.VISIBILITY_VISIBLE);
        values.put(Downloads.Impl.COLUMN_ALLOWED_NETWORK_TYPES, ~0);
        values.put(Downloads.Impl.COLUMN_ALLOW_ROAMING, true);
        values.put(Downloads.Impl.COLUMN_ALLOW_METERED, true);
        values.put(Downloads.Impl.COLUMN_FLAGS, 0);
        values.put(Downloads.Impl.COLUMN_IS_VISIBLE_IN_DOWNLOADS_UI, true);
        return values;
    }

    static long enqueue(Fixture f, DownloadRequests.Caller caller, ContentValues values) {
        Uri uri = f.requests.insert(caller, Downloads.Impl.CONTENT_URI, values);
        return Long.parseLong(uri.getLastPathSegment());
    }

    static String appFile(Fixture f, String name) {
        File dir = new File(f.external, "Android/data/" + APP + "/files/Download");
        dir.mkdirs();
        return Uri.fromFile(new File(dir, name)).toString();
    }

    static <T extends Throwable> T expect(Class<T> type, Runnable action) {
        try {
            action.run();
        } catch (Throwable t) {
            if (type.isInstance(t)) return type.cast(t);
            System.out.println("  unexpected " + t);
            return null;
        }
        return null;
    }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("gesher-downloads-").toFile();
        Clock clock = new Clock();
        DownloadRequests.Caller app = caller(APP_UID, APP, "android.permission.INTERNET");
        DownloadRequests.Caller other = caller(OTHER_UID, OTHER, "android.permission.INTERNET");
        byte[] body = payload(300_000, 7);
        try (Server server = new Server(); Fixture f = new Fixture(root, clock)) {
            f.queue.start();
            server.handlers.put("/file.bin", (r, out) -> respond(out, "200 OK",
                    "Content-Type: application/octet-stream\r\nContent-Length: " + body.length
                            + "\r\nETag: \"v1\"\r\nConnection: close\r\n", body));
            server.handlers.put("/redirect", (r, out) -> respond(out, "302 Found",
                    "Location: /file.bin\r\nContent-Length: 0\r\nConnection: close\r\n", null));
            server.handlers.put("/moved", (r, out) -> respond(out, "301 Moved Permanently",
                    "Location: " + server.url("/file.bin") + "\r\nContent-Length: 0\r\nConnection: close\r\n", null));
            server.handlers.put("/loop", (r, out) -> respond(out, "302 Found",
                    "Location: /loop\r\nContent-Length: 0\r\nConnection: close\r\n", null));
            server.handlers.put("/busy", (r, out) -> respond(out, "503 Service Unavailable",
                    "Retry-After: 5\r\nContent-Length: 0\r\nConnection: close\r\n", null));
            server.handlers.put("/attach", (r, out) -> respond(out, "200 OK",
                    "Content-Type: text/plain; charset=utf-8\r\nContent-Disposition: attachment; filename=\"report.txt\""
                            + "\r\nContent-Length: 5\r\nConnection: close\r\n", "hello".getBytes(StandardCharsets.US_ASCII)));
            AtomicInteger slowStarted = new AtomicInteger();
            server.handlers.put("/slow", (r, out) -> {
                // 300 KB in 4 KB pieces, 40 ms apart: a transfer that is still running.
                String range = r.header("Range");
                long from = range == null ? 0 : Long.parseLong(range.replace("bytes=", "").replace("-", ""));
                byte[] rest = Arrays.copyOfRange(body, (int) from, body.length);
                out.write(((range == null ? "HTTP/1.1 200 OK" : "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                        + from + "-" + (body.length - 1) + "/" + body.length)
                        + "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + rest.length
                        + "\r\nETag: \"v1\"\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
                out.flush();
                slowStarted.incrementAndGet();
                for (int at = 0; at < rest.length; at += 4096) {
                    out.write(rest, at, Math.min(4096, rest.length - at));
                    out.flush();
                    try { Thread.sleep(40); } catch (InterruptedException e) { return; }
                }
            });
            server.handlers.put("/unknown-length", (r, out) -> respond(out, "200 OK",
                    "Content-Type: application/octet-stream\r\n", "partial".getBytes(StandardCharsets.US_ASCII)));
            int half = body.length / 2;
            for (String path : new String[] {"/flaky", "/flaky-no-etag", "/flaky-restart"}) {
                boolean etag = !path.equals("/flaky-no-etag");
                AtomicInteger attempts = new AtomicInteger();
                server.handlers.put(path, (r, out) -> {
                    String range = r.header("Range");
                    if (range == null) {
                        attempts.incrementAndGet();
                        respond(out, "200 OK", "Content-Type: application/octet-stream\r\nContent-Length: "
                                + body.length + (etag ? "\r\nETag: \"v1\"" : "") + "\r\nConnection: close\r\n",
                                Arrays.copyOf(body, half));
                        return; // The socket closes after half of the declared length.
                    }
                    long from = Long.parseLong(range.replace("bytes=", "").replace("-", ""));
                    if (etag && !"\"v1\"".equals(r.header("If-Match"))) {
                        respond(out, "412 Precondition Failed", "Content-Length: 0\r\nConnection: close\r\n", null);
                        return;
                    }
                    byte[] rest = Arrays.copyOfRange(body, (int) from, body.length);
                    respond(out, "206 Partial Content", "Content-Type: application/octet-stream\r\nContent-Range: bytes "
                            + from + "-" + (body.length - 1) + "/" + body.length + "\r\nContent-Length: " + rest.length
                            + (etag ? "\r\nETag: \"v1\"" : "") + "\r\nConnection: close\r\n", rest);
                });
            }

            // 1. Enqueue as DownloadManager does; the bytes, row and broadcast are real.
            long id = enqueue(f, app, request(server.url("/file.bin"), appFile(f, "file.bin"),
                    "X-Test: one", "Accept: */*"));
            check(f.grants.contains(APP + ":" + id), "insert grants the requester all_downloads/<id>");
            f.await(id, Downloads.Impl.STATUS_SUCCESS);
            Map<String, String> row = f.row(app, id);
            check(row != null && "200".equals(row.get("status")), "download completes with STATUS_SUCCESS");
            File local = new File(Uri.parse(appFile(f, "file.bin")).getPath());
            check(local.isFile() && Arrays.equals(Files.readAllBytes(local.toPath()), body),
                    "destination file holds exactly the served bytes");
            check(row != null && Long.toString(body.length).equals(row.get("total_size"))
                    && Long.toString(body.length).equals(row.get("bytes_so_far")), "total_size and bytes_so_far are the real length");
            check(row != null && "application/octet-stream".equals(row.get("media_type")), "media_type comes from Content-Type");
            check(row != null && local.getCanonicalPath().equals(row.get("local_filename")), "local_filename alias maps _data");
            check(row != null && "placeholder".equals(row.get("local_uri")), "local_uri/reason are the projection placeholders DownloadManager translates");
            List<Broadcast> done = f.broadcastsFor(id);
            check(done.size() == 1 && done.get(0).publicApi && APP.equals(done.get(0).packageName),
                    "ACTION_DOWNLOAD_COMPLETE goes once to the requesting package");
            Request sent = server.requests("/file.bin").get(0);
            check("one".equals(sent.header("X-Test")) && "*/*".equals(sent.header("Accept")), "request headers are sent");
            check(sent.header("User-Agent").startsWith("AndroidDownloadManager"), "default User-Agent is DownloadManager's");
            check("identity".equals(sent.header("Accept-Encoding")), "transparent compression is defeated for resumption");

            // 2. Public-API admission (checkInsertPermissions).
            check(expect(SecurityException.class, () -> enqueue(f, caller(APP_UID, APP),
                    request(server.url("/file.bin"), null))) != null, "INTERNET is required to enqueue");
            ContentValues extra = request(server.url("/file.bin"), null);
            extra.put("uid", 0);
            SecurityException invalid = expect(SecurityException.class, () -> enqueue(f, app, extra));
            check(invalid != null && invalid.getMessage().startsWith("Invalid columns in request: uid"),
                    "columns outside the public API are refused");
            ContentValues hidden = request(server.url("/file.bin"), null);
            hidden.put(Downloads.Impl.COLUMN_VISIBILITY, DownloadManager.Request.VISIBILITY_HIDDEN);
            check(expect(SecurityException.class, () -> enqueue(f, app, hidden)) != null,
                    "VISIBILITY_HIDDEN needs DOWNLOAD_WITHOUT_NOTIFICATION");
            ContentValues external = request(server.url("/file.bin"), null);
            external.put(Downloads.Impl.COLUMN_DESTINATION, Downloads.Impl.DESTINATION_EXTERNAL);
            check(expect(SecurityException.class, () -> enqueue(f, app, external)) != null,
                    "DESTINATION_EXTERNAL is not a public-API destination");
            File otherAppDir = new File(f.external, "Android/data/" + OTHER + "/files");
            otherAppDir.mkdirs();
            check(expect(SecurityException.class, () -> enqueue(f, app, request(server.url("/file.bin"),
                    Uri.fromFile(new File(otherAppDir, "x.bin")).toString()))) != null,
                    "another app's external files directory is refused");
            // Path rules compare whole path components of canonical files, with the platform's
            // separator: Windows JVMs canonicalize to backslashes, and Gesher's file layer
            // takes both separators (docs/decisions/0010-paths.md).
            File ownFiles = new File(f.external, "Android/data/" + APP + "/files").getCanonicalFile();
            File sibling = new File(f.external, "Android/data/" + APP + "2/files/x.bin").getCanonicalFile();
            check(DownloadRequests.within(ownFiles, new File(ownFiles, "Download" + File.separator + "file.bin"))
                    && DownloadRequests.within(ownFiles, ownFiles),
                    "a directory contains itself and its descendants (" + File.separator + " separators)");
            check(!DownloadRequests.within(new File(f.external, "Android/data/" + APP).getCanonicalFile(), sibling),
                    "a sibling whose name starts with the package name is not inside it");
            check(f.requests.isFilenameValidInExternalPackage(new File(ownFiles, "Download/file.bin"), APP)
                    && !f.requests.isFilenameValidInExternalPackage(sibling, APP),
                    "the requester's own Android/data directory is its external package directory");
            File ext = f.requests.externalRoot();
            check(f.requests.isFileInPrivateExternalAndroidDirs(new File(ext, "Android/data/" + OTHER + "/files"))
                    && !f.requests.isFileInPrivateExternalAndroidDirs(new File(ext, "Android/data"))
                    && !f.requests.isFileInPrivateExternalAndroidDirs(f.root)
                    && f.requests.isFilenameValidInKnownPublicDir(new File(ext, "Pictures/a.png"))
                    && !f.requests.isFilenameValidInKnownPublicDir(new File(ext, "Pictures")),
                    "Android/(data|obb) and public directories are matched by path components below the volume");
            File escape = new File(ownFiles, "Download");
            String escapeUri = Uri.fromFile(escape).toString() + "/..%5C..%5C..%5C" + OTHER + "%5Cfiles%5Cx.bin";
            check(expect(IllegalArgumentException.class, () -> enqueue(f, app,
                    request(server.url("/file.bin"), escapeUri))) != null,
                    "a parent reference written with backslashes is refused like /../");
            check(expect(IllegalArgumentException.class, () -> f.requests.insert(app,
                    Uri.parse("content://downloads/all_downloads"), request(server.url("/file.bin"), null))) != null,
                    "insert is only accepted on my_downloads");
            check(expect(IllegalArgumentException.class, () -> f.requests.query(app,
                    Uri.parse("content://downloads/public_downloads/1"), null, null, null, null)) != null,
                    "public_downloads is not served by the android-16 provider");

            // 3. Per-UID ownership and strict SQL.
            try (Cursor c = f.dmQuery(other, id)) { check(c.getCount() == 0, "another UID cannot see the download"); }
            check(f.requests.delete(other, Downloads.Impl.CONTENT_URI, "(_id = ? )",
                    new String[] {Long.toString(id)}) == 0, "another UID cannot remove the download");
            check(expect(IllegalArgumentException.class, () -> f.requests.query(app, Downloads.Impl.CONTENT_URI,
                    null, "_id IN (SELECT _id FROM downloads)", null, null)) != null, "subqueries are refused (strict grammar)");
            try (Cursor c = f.requests.query(other, Downloads.Impl.CONTENT_URI, null, "1=1) OR (1=1", null, null)) {
                check(c.getCount() == 0, "a selection cannot escape its parentheses past the UID clause");
            }
            check(expect(IllegalArgumentException.class, () -> f.requests.query(app, Downloads.Impl.CONTENT_URI,
                    new String[] {"sqlite_version()"}, null, null, null)) != null, "unknown projection columns are refused");
            check(expect(SecurityException.class, () -> f.requests.query(app,
                    Uri.parse("content://downloads/my_downloads/" + id + "/headers"), null, null, null, null)) != null,
                    "request headers need ACCESS_ALL_DOWNLOADS");
            boolean otherCannotOpen;
            try { f.requests.openFileTarget(other, ContentUris.withAppendedId(Downloads.Impl.CONTENT_URI, id)); otherCannotOpen = false; }
            catch (FileNotFoundException e) { otherCannotOpen = true; }
            check(otherCannotOpen, "another UID cannot open the downloaded file");
            check(local.getCanonicalFile().equals(f.requests.openFileTarget(app,
                    ContentUris.withAppendedId(Downloads.Impl.CONTENT_URI, id)).file), "the owner opens its file");
            check("application/octet-stream".equals(f.requests.getType(ContentUris.withAppendedId(
                    Downloads.Impl.CONTENT_URI, id))), "getType answers the download's MIME type");

            // 4. Redirects.
            long redirected = enqueue(f, app, request(server.url("/redirect"), appFile(f, "redirected.bin")));
            f.await(redirected, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(redirected) == Downloads.Impl.STATUS_SUCCESS
                    && server.url("/redirect").equals(f.requests.info(redirected).uri), "302 is followed; the URI stays");
            long moved = enqueue(f, app, request(server.url("/moved"), appFile(f, "moved.bin")));
            f.await(moved, Downloads.Impl.STATUS_SUCCESS);
            check(server.url("/file.bin").equals(f.requests.info(moved).uri), "301 records the new URI");
            long loop = enqueue(f, app, request(server.url("/loop"), appFile(f, "loop.bin")));
            f.await(loop, Downloads.Impl.STATUS_TOO_MANY_REDIRECTS);
            check(f.status(loop) == Downloads.Impl.STATUS_TOO_MANY_REDIRECTS
                    && server.requests("/loop").size() == DownloadTransfer.MAX_REDIRECTS, "a redirect loop fails after 5 hops");
            check(f.broadcastsFor(loop).size() == 1, "a failed download is also completed with a broadcast");

            // 5. HTTP failures.
            long missing = enqueue(f, app, request(server.url("/missing"), appFile(f, "missing.bin")));
            f.await(missing, 404);
            check(f.status(missing) == 404 && !new File(Uri.parse(appFile(f, "missing.bin")).getPath()).exists(),
                    "404 is the final status and its reserved file is removed");
            long busy = enqueue(f, app, request(server.url("/busy"), appFile(f, "busy.bin")));
            f.await(busy, Downloads.Impl.STATUS_WAITING_TO_RETRY);
            DownloadRequests.Info busyInfo = f.requests.info(busy);
            check(busyInfo.status == Downloads.Impl.STATUS_WAITING_TO_RETRY && busyInfo.numFailed == 1
                    && busyInfo.retryAfter == DownloadTransfer.MIN_RETRY_AFTER * 1000,
                    "503 waits to retry with Retry-After clamped to 30 s");
            check(f.queue.minimumLatency(busyInfo) >= 30_000, "the retry is delayed, not immediate");
            check(f.broadcastsFor(busy).isEmpty(), "a waiting download is not completed");
            f.requests.delete(app, ContentUris.withAppendedId(Downloads.Impl.CONTENT_URI, busy), null, null);
            check(f.broadcastsFor(busy).size() == 1, "removing an unfinished download sends the completion broadcast");
            long unknown = enqueue(f, app, request(server.url("/unknown-length"), appFile(f, "unknown.bin")));
            f.await(unknown, Downloads.Impl.STATUS_CANNOT_RESUME);
            check(f.status(unknown) == Downloads.Impl.STATUS_CANNOT_RESUME, "a body without a knowable end is not reported complete");

            // 6. Resumption with Range and If-Match after a cut connection.
            long flaky = enqueue(f, app, request(server.url("/flaky"), appFile(f, "flaky.bin")));
            f.await(flaky, Downloads.Impl.STATUS_WAITING_TO_RETRY);
            DownloadRequests.Info cut = f.requests.info(flaky);
            check(cut.status == Downloads.Impl.STATUS_WAITING_TO_RETRY && cut.currentBytes == half
                    && "\"v1\"".equals(cut.etag), "a cut transfer keeps its bytes and ETag and waits to retry");
            clock.offset.addAndGet(10 * 60_000L);
            f.queue.schedule(flaky);
            f.await(flaky, Downloads.Impl.STATUS_SUCCESS);
            List<Request> flakyRequests = server.requests("/flaky");
            Request resume = flakyRequests.get(flakyRequests.size() - 1);
            check(("bytes=" + half + "-").equals(resume.header("Range")) && "\"v1\"".equals(resume.header("If-Match")),
                    "the retry asks for the remaining range with If-Match");
            check(f.status(flaky) == Downloads.Impl.STATUS_SUCCESS && Arrays.equals(Files.readAllBytes(
                    new File(Uri.parse(appFile(f, "flaky.bin")).getPath()).toPath()), body), "the resumed file is complete and exact");
            long noEtag = enqueue(f, app, request(server.url("/flaky-no-etag"), appFile(f, "noetag.bin")));
            f.await(noEtag, Downloads.Impl.STATUS_CANNOT_RESUME);
            check(f.status(noEtag) == Downloads.Impl.STATUS_CANNOT_RESUME
                    && !new File(Uri.parse(appFile(f, "noetag.bin")).getPath()).exists(),
                    "without an ETag a partial download cannot resume and its file is removed");

            // 7. Network requirements are answered from the current network.
            f.network = null;
            int requestsBefore = server.requests("/file.bin").size();
            long offline = enqueue(f, app, request(server.url("/file.bin"), appFile(f, "offline.bin")));
            Thread.sleep(200);
            check(f.status(offline) == Downloads.Impl.STATUS_PENDING
                    && server.requests("/file.bin").size() == requestsBefore, "without a network nothing is requested; the row stays pending");
            f.network = new DownloadQueue.NetworkState(true, false);
            ContentValues unmetered = request(server.url("/file.bin"), appFile(f, "unmetered.bin"));
            unmetered.put(Downloads.Impl.COLUMN_ALLOW_METERED, false);
            long wifiOnly = enqueue(f, app, unmetered);
            f.queue.networkChanged();
            f.await(offline, Downloads.Impl.STATUS_SUCCESS);
            Thread.sleep(200);
            check(f.status(offline) == Downloads.Impl.STATUS_SUCCESS, "a metered network carries an ordinary download");
            check(f.status(wifiOnly) == Downloads.Impl.STATUS_PENDING, "setAllowedOverMetered(false) waits on a metered network");
            f.network = new DownloadQueue.NetworkState(false, true);
            ContentValues noRoaming = request(server.url("/file.bin"), appFile(f, "noroaming.bin"));
            noRoaming.put(Downloads.Impl.COLUMN_ALLOW_ROAMING, false);
            long roaming = enqueue(f, app, noRoaming);
            f.queue.networkChanged();
            f.await(wifiOnly, Downloads.Impl.STATUS_SUCCESS);
            Thread.sleep(200);
            check(f.status(wifiOnly) == Downloads.Impl.STATUS_SUCCESS, "the unmetered download runs once the network is unmetered");
            check(f.status(roaming) == Downloads.Impl.STATUS_PENDING, "setAllowedOverRoaming(false) waits while roaming");
            f.network = new DownloadQueue.NetworkState(false, false);
            f.queue.networkChanged();
            f.await(roaming, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(roaming) == Downloads.Impl.STATUS_SUCCESS, "it runs when the network stops roaming");

            // 8. Cleartext follows the requesting package's network security policy.
            f.cleartext = false;
            long cleartext = enqueue(f, app, request(server.url("/file.bin"), appFile(f, "cleartext.bin")));
            f.await(cleartext, Downloads.Impl.STATUS_BAD_REQUEST);
            check(f.status(cleartext) == Downloads.Impl.STATUS_BAD_REQUEST, "refused cleartext fails with STATUS_BAD_REQUEST");
            f.cleartext = true;

            // 9. Default destination: the provider's cache, names from Content-Disposition, collisions.
            long first = enqueue(f, app, request(server.url("/attach"), null));
            f.await(first, Downloads.Impl.STATUS_SUCCESS);
            long second = enqueue(f, app, request(server.url("/attach"), null));
            f.await(second, Downloads.Impl.STATUS_SUCCESS);
            File firstFile = new File(f.requests.info(first).fileName), secondFile = new File(f.requests.info(second).fileName);
            check(firstFile.getName().equals("report.txt") && firstFile.getParentFile().equals(f.cache),
                    "a cache download is named from Content-Disposition and moved to the cache directory");
            check(secondFile.getName().equals("report-1.txt"), "a colliding name becomes name-1.ext");
            check("text/plain".equals(f.requests.info(first).mimeType), "the MIME type is normalized");

            // 10. Remove deletes the row and the file.
            f.requests.delete(app, Downloads.Impl.CONTENT_URI, "(_id = ? )", new String[] {Long.toString(first)});
            check(f.requests.info(first) == null && !firstFile.exists() && f.revokes.contains(Long.toString(first)),
                    "remove deletes the row, the file and the URI grant");

            // 12. setRequiresCharging / setRequiresDeviceIdle (Helpers.scheduleJob passes them to
            // JobInfo; BatteryController and IdleController decide), from Windows' real state.
            f.power = new int[] {0, 0, 80, 0};  // on battery, discharging
            int chargingBefore = server.requests("/file.bin").size();
            ContentValues charging = request(server.url("/file.bin"), appFile(f, "charging.bin"));
            charging.put(Downloads.Impl.COLUMN_FLAGS, Downloads.Impl.FLAG_REQUIRES_CHARGING);
            long needsPower = enqueue(f, app, charging);
            Thread.sleep(300);
            check(f.status(needsPower) == Downloads.Impl.STATUS_PENDING
                    && server.requests("/file.bin").size() == chargingBefore,
                    "setRequiresCharging(true) on battery: nothing is requested and the row stays pending");
            f.power = new int[] {1, 8 | 4, 3, 0};  // plugged in and charging, but critically low
            check(f.conditionsChanged(), "the queue listens to Windows power and display changes");
            Thread.sleep(300);
            check(f.status(needsPower) == Downloads.Impl.STATUS_PENDING,
                    "a plugged-in battery that is still low is not stable power; the download waits");
            f.power = new int[] {1, 8, 80, 0};  // plugged in and charging
            f.conditionsChanged();
            f.await(needsPower, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(needsPower) == Downloads.Impl.STATUS_SUCCESS,
                    "it starts when the power status changes to charging");
            f.power = new int[] {1, 128, 255, 0};  // a PC without a battery, on AC
            ContentValues desktop = request(server.url("/file.bin"), appFile(f, "desktop.bin"));
            desktop.put(Downloads.Impl.COLUMN_FLAGS, Downloads.Impl.FLAG_REQUIRES_CHARGING);
            long onMains = enqueue(f, app, desktop);
            f.await(onMains, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(onMains) == Downloads.Impl.STATUS_SUCCESS,
                    "a PC without a battery on AC power satisfies the charging requirement");
            f.power = null;  // Windows did not answer
            ContentValues unknownPower = request(server.url("/file.bin"), appFile(f, "unknownpower.bin"));
            unknownPower.put(Downloads.Impl.COLUMN_FLAGS, Downloads.Impl.FLAG_REQUIRES_CHARGING);
            long powerUnknown = enqueue(f, app, unknownPower);
            Thread.sleep(300);
            check(f.status(powerUnknown) == Downloads.Impl.STATUS_PENDING,
                    "an unknown power status does not satisfy the charging requirement");
            f.power = new int[] {1, 8, 80, 0};
            f.conditionsChanged();
            f.await(powerUnknown, Downloads.Impl.STATUS_SUCCESS);

            // Losing the requirement stops a running transfer, as JobScheduler's onStopJob does;
            // it keeps its bytes and starts again once charging and its retry delay allow.
            ContentValues stopping = request(server.url("/slow"), appFile(f, "stopping.bin"));
            stopping.put(Downloads.Impl.COLUMN_FLAGS, Downloads.Impl.FLAG_REQUIRES_CHARGING);
            long unplugged = enqueue(f, app, stopping);
            long startWait = System.currentTimeMillis() + 10_000;
            while (slowStarted.get() == 0 && System.currentTimeMillis() < startWait) Thread.sleep(10);
            Thread.sleep(400);
            f.power = new int[] {0, 0, 80, 0};
            f.conditionsChanged();
            f.await(unplugged, Downloads.Impl.STATUS_WAITING_TO_RETRY, Downloads.Impl.STATUS_SUCCESS);
            DownloadRequests.Info stopped = f.requests.info(unplugged);
            check(stopped.status == Downloads.Impl.STATUS_WAITING_TO_RETRY && stopped.currentBytes > 0
                    && stopped.currentBytes < body.length,
                    "unplugging stops the running transfer with a retryable status and keeps its bytes");
            int slowRequests = server.requests("/slow").size();
            clock.offset.addAndGet(3_600_000);  // past the retry delay
            f.queue.schedule(unplugged);
            Thread.sleep(300);
            check(f.status(unplugged) == Downloads.Impl.STATUS_WAITING_TO_RETRY
                    && server.requests("/slow").size() == slowRequests,
                    "after its retry delay it still waits while on battery");
            f.power = new int[] {1, 8, 80, 0};
            f.conditionsChanged();
            f.await(unplugged, Downloads.Impl.STATUS_SUCCESS);
            List<Request> resumed = server.requests("/slow");
            check(f.status(unplugged) == Downloads.Impl.STATUS_SUCCESS
                    && resumed.get(resumed.size() - 1).header("Range") != null
                    && Arrays.equals(Files.readAllBytes(new File(Uri.parse(appFile(f, "stopping.bin")).getPath()).toPath()), body),
                    "plugged in again, it resumes with a Range request and completes exactly");

            // Device idle: the session display has been off without input for the AOSP
            // inactivity threshold (config_jobSchedulerInactivityIdleThreshold, 31 minutes).
            f.displayOffSince = -1;  // the user is active
            int idleBefore = server.requests("/file.bin").size();
            ContentValues idle = request(server.url("/file.bin"), appFile(f, "idle.bin"));
            idle.put(Downloads.Impl.COLUMN_FLAGS, Downloads.Impl.FLAG_REQUIRES_DEVICE_IDLE);
            long needsIdle = enqueue(f, app, idle);
            Thread.sleep(300);
            check(f.status(needsIdle) == Downloads.Impl.STATUS_PENDING
                    && server.requests("/file.bin").size() == idleBefore,
                    "setRequiresDeviceIdle(true) while the user is active: the row stays pending");
            f.displayOffSince = clock.now() - 60_000;  // display off for a minute
            f.conditionsChanged();
            Thread.sleep(300);
            check(f.status(needsIdle) == Downloads.Impl.STATUS_PENDING,
                    "a display that went off a minute ago is not yet idle");
            f.displayOffSince = clock.now() - (31 * 60_000 - 700);  // idle in 0.7 s, with no further event
            f.conditionsChanged();
            Thread.sleep(200);
            check(f.status(needsIdle) == Downloads.Impl.STATUS_PENDING, "it waits for the rest of the threshold");
            f.await(needsIdle, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(needsIdle) == Downloads.Impl.STATUS_SUCCESS,
                    "it starts once the inactivity threshold passes, without another change notification");
            f.displayOffSince = -1;
            ContentValues both = request(server.url("/file.bin"), appFile(f, "both.bin"));
            both.put(Downloads.Impl.COLUMN_FLAGS,
                    Downloads.Impl.FLAG_REQUIRES_CHARGING | Downloads.Impl.FLAG_REQUIRES_DEVICE_IDLE);
            long needsBoth = enqueue(f, app, both);
            f.displayOffSince = clock.now() - 31 * 60_000;
            f.power = new int[] {0, 0, 80, 0};
            f.conditionsChanged();
            Thread.sleep(300);
            check(f.status(needsBoth) == Downloads.Impl.STATUS_PENDING, "idle on battery does not satisfy both requirements");
            f.power = new int[] {1, 8, 80, 0};
            f.conditionsChanged();
            f.await(needsBoth, Downloads.Impl.STATUS_SUCCESS);
            check(f.status(needsBoth) == Downloads.Impl.STATUS_SUCCESS, "idle and charging together start it");
            f.displayOffSince = -1;

            // 11. Persistence and resumption across a Gesher restart.
            long restart = enqueue(f, app, request(server.url("/flaky-restart"), appFile(f, "restart.bin")));
            f.await(restart, Downloads.Impl.STATUS_WAITING_TO_RETRY);
            f.queue.shutdown();
            // The process ended while the retry was running.
            f.db.execSQL("UPDATE downloads SET status = 192 WHERE _id = " + restart);
            f.db.close();
            f.open();
            check(f.requests.info(id) != null && f.status(id) == Downloads.Impl.STATUS_SUCCESS,
                    "completed downloads persist across restarts");
            f.queue.start();
            f.await(restart, Downloads.Impl.STATUS_SUCCESS);
            List<Request> restarted = server.requests("/flaky-restart");
            check(f.status(restart) == Downloads.Impl.STATUS_SUCCESS
                    && ("bytes=" + half + "-").equals(restarted.get(restarted.size() - 1).header("Range"))
                    && Arrays.equals(Files.readAllBytes(new File(Uri.parse(appFile(f, "restart.bin")).getPath()).toPath()), body),
                    "a download cut off by a restart resumes from its durable offset");
            check(f.broadcastsFor(restart).size() == 1, "the resumed download completes once");
        } finally {
            deleteTree(root);
        }
        if (failures != 0) {
            System.out.println(failures + " download contract(s) failed");
            System.exit(1);
        }
        System.out.println("All download contracts passed");
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
}
