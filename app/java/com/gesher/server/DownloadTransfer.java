/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.provider.Downloads;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.ProtocolException;
import java.net.URL;
import java.security.GeneralSecurityException;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * One execution of one DownloadManager request: the HTTP part of AOSP DownloadThread
 * (android-16.0.0_r4 packages/providers/DownloadProvider). It runs on the system
 * server's own HttpURLConnection, which is the platform stack apps use: Windows' proxy
 * settings ({@code WindowsProxy}) and the Windows certificate store behind the system
 * trust anchors. The caller's network-security policy decides cleartext and TLS trust.
 *
 * <p>Statuses, retry accounting, redirect limits, Range/If-Match resumption and the
 * final destination handling follow DownloadThread. Nothing here reports success
 * without the bytes the server actually sent: a length mismatch, an unresumable
 * partial file or an unknown end of body is an error, as on Android.
 */
final class DownloadTransfer {
    /** Constants.MAX_REDIRECTS, BUFFER_SIZE, MIN_PROGRESS_STEP/TIME, MAX_RETRIES. */
    static final int MAX_REDIRECTS = 5;
    static final int BUFFER_SIZE = 8192;
    static final long MIN_PROGRESS_STEP = 65536;
    static final long MIN_PROGRESS_TIME = 2000;
    static final int MAX_RETRIES = 5;
    /** Constants.MIN_RETRY_AFTER / MAX_RETRY_AFTER, in seconds. */
    static final int MIN_RETRY_AFTER = 30, MAX_RETRY_AFTER = 24 * 60 * 60;
    /** DownloadThread.DEFAULT_TIMEOUT. */
    static final int DEFAULT_TIMEOUT = 20_000;
    private static final int HTTP_TEMP_REDIRECT = 307;
    private static final int HTTP_REQUESTED_RANGE_NOT_SATISFIABLE = 416;
    private static final int MAX_RANGES_PER_RUN = 32;
    private static final Pattern CONTENT_RANGE = Pattern.compile(
            "bytes ([0-9]+)-([0-9]+)/([0-9]+|\\*)", Pattern.CASE_INSENSITIVE);

    /** DownloadThread.DownloadInfoDelta: the mutable part of a download's row. */
    static final class State {
        String uri;
        String fileName;
        String mimeType;
        int status;
        int numFailed;
        int retryAfter;
        long totalBytes = -1;
        long currentBytes;
        String etag;
        String errorMsg;
    }

    /** The network, trust and network-security decisions for the requesting package. */
    interface Network {
        /**
         * Null when a network the request may use is connected; otherwise the reason the
         * request waits (DownloadThread.checkConnectivity: disconnected, roaming, metered).
         */
        String refusal(long totalBytes);
        /** Whether the request may use a metered network at this size (DownloadInfo). */
        boolean meteredAllowed(long totalBytes);
        boolean cleartextPermitted(String host);
        javax.net.ssl.SSLSocketFactory socketFactory() throws GeneralSecurityException;
        HttpURLConnection open(URL url) throws IOException;
    }

    /** The row's storage, owned by the provider. */
    interface Row {
        List<String[]> headers();
        String userAgent();
        /** Helpers.generateSaveFile: chooses and reserves the destination. */
        File reserve(String contentDisposition, String contentLocation, String mimeType)
                throws IOException;
        /** Helpers.getSuccessDestinationDirectory move for non-FILE_URI destinations. */
        File finalLocation(File running);
        /**
         * DownloadInfoDelta.writeToDatabaseOrThrow: false when the row was paused,
         * canceled or deleted; {@link #paused} then distinguishes them.
         */
        boolean checkpoint(State state);
        boolean paused();
        long now();
    }

    static final class Stop extends Exception {
        final int status;
        Stop(int status, String message) { super(message); this.status = status; }
        Stop(int status, String message, Throwable cause) { super(message, cause); this.status = status; }
    }

    private final State mState;
    private final Network mNetwork;
    private final Row mRow;
    private final long mInitialBytes;
    private volatile boolean mShutdown;
    private volatile boolean mPolicyDirty;
    private long mDurableBytes;
    private long mLastUpdateBytes, mLastUpdateTime;

    DownloadTransfer(State state, Network network, Row row) {
        mState = state;
        mNetwork = network;
        mRow = row;
        mInitialBytes = mDurableBytes = mLastUpdateBytes = state.currentBytes;
    }

    /** DownloadThread.requestShutdown: the running request stops with a retryable error. */
    void requestShutdown() { mShutdown = true; }

    /** As DownloadThread's policy-dirty flag: recheck before receiving more data. */
    void networkChanged() { mPolicyDirty = true; }

    /**
     * DownloadThread.run without the JobService: returns with {@code mState.status} set to
     * the status to write; waiting statuses mean the queue runs it again later.
     */
    void run() {
        try {
            mState.status = Downloads.Impl.STATUS_RUNNING;
            execute();
            mState.status = Downloads.Impl.STATUS_SUCCESS;
            if (mState.totalBytes == -1) mState.totalBytes = mState.currentBytes;
        } catch (Stop e) {
            mState.currentBytes = mDurableBytes;
            mState.status = e.status;
            mState.errorMsg = e.getMessage();
            if (isStatusRetryable(mState.status)) {
                boolean madeProgress = mDurableBytes > mInitialBytes;
                if (madeProgress) mState.numFailed = 1;
                else mState.numFailed += 1;
                if (mState.numFailed < MAX_RETRIES) {
                    mState.status = mNetwork.refusal(mState.totalBytes) == null
                            ? Downloads.Impl.STATUS_WAITING_TO_RETRY
                            : Downloads.Impl.STATUS_WAITING_FOR_NETWORK;
                    if (mState.currentBytes != 0 && !isStrongEtag(mState.etag)) {
                        // Data was written and nothing can verify a later range against it.
                        mState.status = Downloads.Impl.STATUS_CANNOT_RESUME;
                    }
                }
            }
            if (mState.status == Downloads.Impl.STATUS_WAITING_FOR_NETWORK
                    && !mNetwork.meteredAllowed(mState.totalBytes)) {
                mState.status = Downloads.Impl.STATUS_QUEUED_FOR_WIFI;
            }
        } catch (RuntimeException | Error t) {
            mState.currentBytes = mDurableBytes;
            mState.status = Downloads.Impl.STATUS_UNKNOWN_ERROR;
            mState.errorMsg = t.toString();
        } finally {
            finalizeDestination();
        }
    }

    static boolean isStatusRetryable(int status) {
        switch (status) {
            case Downloads.Impl.STATUS_HTTP_DATA_ERROR:
            case HttpURLConnection.HTTP_UNAVAILABLE:
            case HttpURLConnection.HTTP_INTERNAL_ERROR:
            case Downloads.Impl.STATUS_FILE_ERROR:
                return true;
            default:
                return false;
        }
    }

    private void execute() throws Stop {
        URL url;
        try {
            url = new URL(mState.uri);
        } catch (MalformedURLException e) {
            throw new Stop(Downloads.Impl.STATUS_BAD_REQUEST, e.getMessage(), e);
        }
        int redirects = 0;
        int ranges = 0;
        while (true) {
            checkShutdown();
            boolean resuming = mState.currentBytes != 0;
            if (resuming && !isStrongEtag(mState.etag)) {
                throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Partial file has no strong ETag");
            }
            String protocol = url.getProtocol();
            if (!"http".equalsIgnoreCase(protocol) && !"https".equalsIgnoreCase(protocol)) {
                throw new Stop(Downloads.Impl.STATUS_BAD_REQUEST, "Unsupported scheme " + protocol);
            }
            // Enforced per hop: a redirect may change HTTPS to HTTP.
            if ("http".equalsIgnoreCase(protocol) && !mNetwork.cleartextPermitted(url.getHost())) {
                throw new Stop(Downloads.Impl.STATUS_BAD_REQUEST,
                        "Cleartext traffic not permitted for the requesting package");
            }
            HttpURLConnection conn = null;
            try {
                checkConnectivity();
                conn = mNetwork.open(url);
                conn.setConnectTimeout(DEFAULT_TIMEOUT);
                conn.setInstanceFollowRedirects(false);
                conn.setReadTimeout(DEFAULT_TIMEOUT);
                if (conn instanceof javax.net.ssl.HttpsURLConnection) {
                    try {
                        ((javax.net.ssl.HttpsURLConnection) conn).setSSLSocketFactory(
                                mNetwork.socketFactory());
                    } catch (GeneralSecurityException e) {
                        throw new Stop(Downloads.Impl.STATUS_UNKNOWN_ERROR,
                                "Unable to create SSLContext.", e);
                    }
                }
                addRequestHeaders(conn, resuming);
                int code = conn.getResponseCode();
                switch (code) {
                    case HttpURLConnection.HTTP_OK:
                        if (resuming) {
                            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME,
                                    "Expected partial, but received OK");
                        }
                        parseOkHeaders(conn);
                        transferData(conn, mState.totalBytes, false);
                        return;
                    case HttpURLConnection.HTTP_PARTIAL: {
                        if (!resuming) {
                            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME,
                                    "Expected OK, but received partial");
                        }
                        long end = parsePartialHeaders(conn);
                        checkConnectivity();
                        transferData(conn, end, true);
                        if (end == mState.totalBytes) return;
                        checkpointOrThrow();
                        if (++ranges == MAX_RANGES_PER_RUN) {
                            throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR,
                                    "More ranges remain after the transfer work limit");
                        }
                        continue;
                    }
                    case HttpURLConnection.HTTP_MOVED_PERM:
                    case HttpURLConnection.HTTP_MOVED_TEMP:
                    case HttpURLConnection.HTTP_SEE_OTHER:
                    case HTTP_TEMP_REDIRECT: {
                        if (++redirects >= MAX_REDIRECTS) {
                            throw new Stop(Downloads.Impl.STATUS_TOO_MANY_REDIRECTS, "Too many redirects");
                        }
                        String location = conn.getHeaderField("Location");
                        if (location == null) {
                            throw new Stop(Downloads.Impl.STATUS_UNHANDLED_REDIRECT,
                                    "Redirect without a location");
                        }
                        url = new URL(url, location);
                        if (code == HttpURLConnection.HTTP_MOVED_PERM) mState.uri = url.toString();
                        continue;
                    }
                    case HttpURLConnection.HTTP_PRECON_FAILED:
                        throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Precondition failed");
                    case HTTP_REQUESTED_RANGE_NOT_SATISFIABLE:
                        throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME,
                                "Requested range not satisfiable");
                    case HttpURLConnection.HTTP_UNAVAILABLE:
                        parseUnavailableHeaders(conn);
                        throw new Stop(HttpURLConnection.HTTP_UNAVAILABLE, conn.getResponseMessage());
                    case HttpURLConnection.HTTP_INTERNAL_ERROR:
                        throw new Stop(HttpURLConnection.HTTP_INTERNAL_ERROR, conn.getResponseMessage());
                    default:
                        throw unhandled(code, conn.getResponseMessage());
                }
            } catch (IOException e) {
                if (e instanceof ProtocolException && e.getMessage() != null
                        && e.getMessage().startsWith("Unexpected status line")) {
                    throw new Stop(Downloads.Impl.STATUS_UNHANDLED_HTTP_CODE, e.getMessage(), e);
                }
                throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR, String.valueOf(e), e);
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
    }

    /** StopRequestException.throwUnhandledHttpError; HTTP 308 is unhandled there too. */
    static Stop unhandled(int code, String message) {
        String error = "Unhandled HTTP response: " + code + " " + message;
        if (code >= 400 && code < 600) return new Stop(code, error);
        if (code >= 300 && code < 400) return new Stop(Downloads.Impl.STATUS_UNHANDLED_REDIRECT, error);
        return new Stop(Downloads.Impl.STATUS_UNHANDLED_HTTP_CODE, error);
    }

    private void addRequestHeaders(HttpURLConnection conn, boolean resuming) {
        for (String[] header : mRow.headers()) conn.addRequestProperty(header[0], header[1]);
        if (conn.getRequestProperty("User-Agent") == null) {
            conn.addRequestProperty("User-Agent", mRow.userAgent());
        }
        // Defeat transparent gzip, which prevents resuming, and connection reuse.
        conn.setRequestProperty("Accept-Encoding", "identity");
        conn.setRequestProperty("Connection", "close");
        if (resuming) {
            conn.setRequestProperty("If-Match", mState.etag);
            conn.setRequestProperty("Range", "bytes=" + mState.currentBytes + "-");
        }
    }

    private void parseOkHeaders(HttpURLConnection conn) throws Stop {
        if (mState.fileName == null) {
            try {
                if (mState.mimeType == null) mState.mimeType = normalizeMimeType(conn.getContentType());
                File file = mRow.reserve(conn.getHeaderField("Content-Disposition"),
                        conn.getHeaderField("Content-Location"), mState.mimeType);
                mState.fileName = file.getAbsolutePath();
            } catch (IOException e) {
                throw new Stop(Downloads.Impl.STATUS_FILE_ERROR, "Failed to generate filename: " + e, e);
            }
        }
        if (mState.mimeType == null) mState.mimeType = normalizeMimeType(conn.getContentType());
        mState.totalBytes = conn.getHeaderField("Transfer-Encoding") == null
                ? headerLong(conn, "Content-Length", -1) : -1;
        mState.etag = identityEncoded(conn) ? conn.getHeaderField("ETag") : null;
        checkpointOrThrow();
        checkConnectivity();
    }

    private long parsePartialHeaders(HttpURLConnection conn) throws Stop {
        String etag = conn.getHeaderField("ETag");
        if (etag != null && !etag.equals(mState.etag)) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Partial response changed the ETag");
        }
        if (!identityEncoded(conn)
                || "multipart/byteranges".equals(normalizeMimeType(conn.getContentType()))) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Expected a single identity range");
        }
        String header = conn.getHeaderField("Content-Range");
        Matcher match = CONTENT_RANGE.matcher(header == null ? "" : header.trim());
        if (!match.matches()) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Missing or invalid Content-Range");
        }
        try {
            long first = Long.parseLong(match.group(1));
            long last = Long.parseLong(match.group(2));
            String size = match.group(3);
            long total = "*".equals(size) ? mState.totalBytes : Long.parseLong(size);
            String length = conn.getHeaderField("Content-Length");
            if (first != mState.currentBytes || last < first || last >= total
                    || (mState.totalBytes != -1 && mState.totalBytes != total)
                    || (length != null && Long.parseLong(length) != last - first + 1)) {
                throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Inconsistent Content-Range");
            }
            mState.totalBytes = total;
            return last + 1;
        } catch (NumberFormatException e) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Invalid range size", e);
        }
    }

    private static boolean identityEncoded(HttpURLConnection conn) {
        String encoding = conn.getHeaderField("Content-Encoding");
        return encoding == null || "identity".equalsIgnoreCase(encoding.trim());
    }

    private static boolean isStrongEtag(String value) {
        if (value == null || value.length() < 2 || value.charAt(0) != '"'
                || value.charAt(value.length() - 1) != '"') return false;
        for (int i = 1; i < value.length() - 1; i++) {
            char c = value.charAt(i);
            if (c <= ' ' || c == '"' || c == 0x7f || c > 0xff) return false;
        }
        return true;
    }

    /** Intent.normalizeMimeType: lower case, without parameters. */
    static String normalizeMimeType(String type) {
        if (type == null) return null;
        type = type.trim().toLowerCase(java.util.Locale.ROOT);
        int semicolon = type.indexOf(';');
        return semicolon == -1 ? type : type.substring(0, semicolon);
    }

    private void parseUnavailableHeaders(HttpURLConnection conn) {
        long retryAfter = conn.getHeaderFieldInt("Retry-After", -1);
        retryAfter = Math.max(MIN_RETRY_AFTER, Math.min(MAX_RETRY_AFTER, retryAfter));
        mState.retryAfter = (int) (retryAfter * 1000);
    }

    private void transferData(HttpURLConnection conn, long end, boolean range) throws Stop {
        boolean close = "close".equalsIgnoreCase(conn.getHeaderField("Connection"));
        boolean chunked = "chunked".equalsIgnoreCase(conn.getHeaderField("Transfer-Encoding"));
        if (end == -1 && !close && !chunked) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "can't know size of download, giving up");
        }
        if (mState.fileName == null) {
            throw new Stop(Downloads.Impl.STATUS_CANNOT_RESUME, "Partial download has no file");
        }
        InputStream in;
        try {
            in = conn.getInputStream();
        } catch (IOException e) {
            throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR, String.valueOf(e), e);
        }
        File file = new File(mState.fileName);
        try (InputStream body = in) {
            // Never recreate a missing partial file or trust bytes beyond its checkpoint.
            if (!file.isFile() || file.length() < mState.currentBytes) {
                throw new Stop(mState.currentBytes != 0 ? Downloads.Impl.STATUS_CANNOT_RESUME
                        : Downloads.Impl.STATUS_FILE_ERROR, "Partial file is missing or shorter than recorded");
            }
            try (FileOutputStream output = new FileOutputStream(file, true)) {
                output.getChannel().truncate(mState.currentBytes);
                output.getChannel().position(mState.currentBytes);
                if (mState.totalBytes > 0 && file.getParentFile() != null
                        && file.getParentFile().getUsableSpace() < mState.totalBytes - mState.currentBytes) {
                    throw new Stop(Downloads.Impl.STATUS_INSUFFICIENT_SPACE_ERROR,
                            "Insufficient space for the reported length");
                }
                try {
                    copyBody(body, output, end, range);
                } catch (Stop failure) {
                    if (!Downloads.Impl.isStatusError(failure.status) || isStatusRetryable(failure.status)) {
                        try {
                            syncProgress(output);
                        } catch (IOException syncFailure) {
                            failure.addSuppressed(syncFailure);
                        }
                    }
                    throw failure;
                }
                syncProgress(output);
            }
        } catch (IOException closeFailure) {
            throw new Stop(Downloads.Impl.STATUS_FILE_ERROR, String.valueOf(closeFailure), closeFailure);
        }
    }

    private void copyBody(InputStream body, FileOutputStream output, long end, boolean range)
            throws IOException, Stop {
        byte[] buffer = new byte[BUFFER_SIZE];
        while (true) {
            if (mPolicyDirty) {
                // Clear before querying: a later notification must remain pending.
                mPolicyDirty = false;
                checkConnectivity();
            }
            checkShutdown();
            int length;
            try {
                length = body.read(buffer);
            } catch (IOException e) {
                throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR, "Failed reading response: " + e, e);
            }
            if (length == -1) break;
            if (end != -1 && length > end - mState.currentBytes) {
                throw new Stop(range ? Downloads.Impl.STATUS_CANNOT_RESUME
                        : Downloads.Impl.STATUS_HTTP_DATA_ERROR, "Response exceeded its declared range");
            }
            output.write(buffer, 0, length);
            mState.currentBytes += length;
            updateProgress(output);
        }
        if (end != -1 && mState.currentBytes != end) {
            throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR, "Content length mismatch; found "
                    + mState.currentBytes + " instead of " + end);
        }
    }

    private void checkShutdown() throws Stop {
        if (mShutdown) {
            throw new Stop(Downloads.Impl.STATUS_HTTP_DATA_ERROR, "Local halt requested");
        }
    }

    private void syncProgress(FileOutputStream output) throws IOException {
        if (mState.currentBytes != mDurableBytes) {
            output.getFD().sync();
            mDurableBytes = mState.currentBytes;
        }
    }

    private void updateProgress(FileOutputStream out) throws IOException, Stop {
        long now = mRow.now();
        long bytesDelta = mState.currentBytes - mLastUpdateBytes;
        long timeDelta = now - mLastUpdateTime;
        if (bytesDelta > MIN_PROGRESS_STEP && timeDelta > MIN_PROGRESS_TIME) {
            // Persist only bytes that are durable, so a resume never trusts lost data.
            syncProgress(out);
            checkpointOrThrow();
            mLastUpdateBytes = mState.currentBytes;
            mLastUpdateTime = now;
        }
    }

    private void checkpointOrThrow() throws Stop {
        if (!mRow.checkpoint(mState)) {
            if (mRow.paused()) throw new Stop(Downloads.Impl.STATUS_PAUSED_BY_APP, "Download paused!");
            throw new Stop(Downloads.Impl.STATUS_CANCELED, "Download deleted or missing!");
        }
    }

    private void checkConnectivity() throws Stop {
        String refusal = mNetwork.refusal(mState.totalBytes);
        if (refusal != null) throw new Stop(Downloads.Impl.STATUS_WAITING_FOR_NETWORK, refusal);
    }

    /** DownloadThread.finalizeDestination. */
    private void finalizeDestination() {
        if (Downloads.Impl.isStatusError(mState.status)) {
            if (mState.fileName != null) {
                new File(mState.fileName).delete();
                mState.fileName = null;
            }
        } else if (Downloads.Impl.isStatusSuccess(mState.status) && mState.fileName != null) {
            File before = new File(mState.fileName);
            File after = mRow.finalLocation(before);
            if (after != null && !after.equals(before) && before.renameTo(after)) {
                mState.fileName = after.getAbsolutePath();
            }
        }
    }

    private static long headerLong(HttpURLConnection conn, String field, long fallback) {
        try {
            return Long.parseLong(conn.getHeaderField(field));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }
}
