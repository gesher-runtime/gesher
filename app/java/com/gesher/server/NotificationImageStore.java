/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.DigestOutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Semaphore;

/**
 * Synchronous preparation of notification images, separate from publication.
 *
 * There are at most four encoders and 256 optional fingerprint entries. The cache
 * retains neither Bitmaps nor PNG bodies, and owns no additional on-disk copies.
 * A published PNG belongs to a retained Windows notification, including after this
 * process exits: cache eviction must never delete it. Callers serialize publish
 * and deletion with their notification lifecycle fence. Unpublished files are
 * owned by Prepared and removed on every unsuccessful/stale publication path.
 */
final class NotificationImageStore {
    static final int MAX_ENCODERS = 4;
    static final int MAX_CACHE_ENTRIES = 256;
    private static final Semaphore ENCODERS = new Semaphore(MAX_ENCODERS, true);
    private static final Object LOCK = new Object();
    private static final Map<String, Lane> LANES = new HashMap<>();
    private static final LinkedHashMap<String, Cached> CACHE = new LinkedHashMap<>(16, .75f, true);

    interface Image {
        /** Actual content and format, or null when an exact reusable signature is unavailable. */
        byte[] signature() throws IOException;
        boolean encode(OutputStream out) throws IOException;
        /** False if content changed while signature/encoding was in progress. */
        boolean unchanged();
    }

    private static final class Lane { int users; boolean busy, removeWhenIdle; }
    private static final class Cached {
        final WeakReference<Object> owner;
        final boolean systemOwner;
        final byte[] source, png;
        final long size;
        Cached(Object owner, byte[] source, byte[] png, long size) {
            this.owner = new WeakReference<>(owner);
            systemOwner = owner == null;
            this.source = source.clone();
            this.png = png.clone();
            this.size = size;
        }
        boolean matches(Object current, byte[] fingerprint) {
            return (current == null ? systemOwner : !systemOwner && owner.get() == current)
                    && Arrays.equals(source, fingerprint);
        }
    }

    private NotificationImageStore() { }

    static Prepared prepare(File target, Object owner, Image image) throws IOException, InterruptedException {
        String key = target.getAbsolutePath();
        Lane lane;
        synchronized (LOCK) {
            lane = LANES.computeIfAbsent(key, ignored -> new Lane());
            lane.users++;
            try {
                while (lane.busy) LOCK.wait();
                lane.busy = true;
            } catch (InterruptedException failure) {
                if (--lane.users == 0) LANES.remove(key);
                throw failure;
            }
        }
        boolean permit = false;
        Prepared prepared = null;
        try {
            ENCODERS.acquire();
            permit = true;
            prepared = new Prepared(target, key, lane);
            if (image == null) return prepared;
            byte[] signature = image.signature();
            Cached cached;
            synchronized (LOCK) { cached = CACHE.get(key); }
            if (signature != null && cached != null && cached.matches(owner, signature)
                    && matchesFile(target, cached.png, cached.size) && image.unchanged()) {
                prepared.cached = cached;
                return prepared;
            }
            File parent = target.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
                throw new IOException("Cannot create notification image directory");
            }
            prepared.staged = File.createTempFile("notification-image-", ".part", parent);
            MessageDigest digest = sha256();
            boolean encoded;
            try (OutputStream out = new BufferedOutputStream(new DigestOutputStream(
                    new FileOutputStream(prepared.staged), digest), 8192)) {
                encoded = image.encode(out);
            }
            if (!encoded) {
                prepared.discardStage();
                return prepared;
            }
            // An encoder may have consumed concurrently changing pixels. Publish
            // its result as before, but never reuse a mismatching pre-encode key.
            if (signature != null && image.unchanged()) {
                prepared.cached = new Cached(owner, signature, digest.digest(), prepared.staged.length());
            }
            return prepared;
        } catch (IOException | RuntimeException | Error | InterruptedException failure) {
            if (prepared != null) prepared.close();
            else {
                if (permit) ENCODERS.release();
                releaseLane(key, lane);
            }
            throw failure;
        }
    }

    static final class Prepared implements AutoCloseable {
        private final File target;
        private final String key;
        private final Lane lane;
        private File staged;
        private Cached cached;
        private boolean closed, published;

        private Prepared(File target, String key, Lane lane) {
            this.target = target; this.key = key; this.lane = lane;
        }

        /** Called only after owner/channel/delegate/cancellation revalidation. */
        File publish() throws IOException {
            if (closed || published) throw new IllegalStateException("Image preparation is already consumed");
            published = true;
            if (staged == null) {
                // Same-key preparations hold their lane until close, so another
                // post cannot replace a cache hit between prepare and publish.
                // Cancellation/deletion is independently fenced by the caller.
                if (cached == null || !target.isFile()) return null;
                synchronized (LOCK) { lane.removeWhenIdle = false; }
                return target;
            }
            // Both paths are in the same directory. Failure must retain the old
            // published image; do not delete it to force a non-atomic fallback.
            Files.move(staged.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
            staged = null;
            synchronized (LOCK) {
                lane.removeWhenIdle = false;
                if (cached == null) CACHE.remove(key);
                else {
                    CACHE.put(key, cached);
                    while (CACHE.size() > MAX_CACHE_ENTRIES) CACHE.remove(CACHE.keySet().iterator().next());
                }
            }
            return target;
        }

        private void discardStage() throws IOException {
            if (staged != null) {
                Files.deleteIfExists(staged.toPath());
                staged = null;
            }
        }

        @Override public void close() {
            if (closed) return;
            closed = true;
            try { discardStage(); }
            catch (IOException failure) {
                // Filesystem refusal cannot be repaired by dropping ownership
                // while an encoder is running. At this point it is closed; the
                // explicit .part name cannot be mistaken for a published image.
                SystemServer.log("notification: cannot remove staged image: " + failure);
            } finally {
                ENCODERS.release();
                releaseLane(key, lane);
            }
        }
    }

    private static void releaseLane(String key, Lane lane) {
        synchronized (LOCK) {
            if (!lane.removeWhenIdle) {
                finishLane(key, lane);
                return;
            }
        }
        // Keep the same-key lane busy across deletion. A new preparation must
        // not borrow the old file just before this last owner removes it.
        deleteFile(new File(key));
        synchronized (LOCK) { finishLane(key, lane); }
    }

    private static void finishLane(String key, Lane lane) {
        lane.busy = false;
        lane.removeWhenIdle = false;
        if (--lane.users == 0) LANES.remove(key);
        LOCK.notifyAll();
    }

    /** Caller holds the same lifecycle fence as Prepared.publish. Never joins an encoder. */
    static void remove(File target) {
        String key = target.getAbsolutePath();
        Lane lane;
        synchronized (LOCK) {
            CACHE.remove(key);
            lane = LANES.get(key);
            if (lane != null && lane.busy) {
                lane.removeWhenIdle = true;
                return;
            }
            // A released lane can still have notified waiters which have not
            // reacquired LOCK. Keep that same lane so they cannot later run
            // alongside an unrelated replacement lane for this pathname.
            if (lane == null) {
                lane = new Lane();
                LANES.put(key, lane);
            }
            lane.users++; lane.busy = true; lane.removeWhenIdle = true;
        }
        releaseLane(key, lane);
    }

    private static void deleteFile(File target) {
        try { Files.deleteIfExists(target.toPath()); }
        catch (IOException failure) { SystemServer.log("notification: cannot remove image: " + failure); }
    }

    private static boolean matchesFile(File file, byte[] expected, long size) {
        if (!file.isFile() || file.length() != size) return false;
        try (FileInputStream input = new FileInputStream(file)) {
            MessageDigest digest = sha256();
            byte[] block = new byte[8192];
            long read = 0;
            for (int count; (count = input.read(block)) != -1;) {
                read += count;
                if (read > size) return false;
                digest.update(block, 0, count);
            }
            return read == size && Arrays.equals(digest.digest(), expected);
        } catch (IOException failure) { return false; }
    }

    static MessageDigest sha256() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }
}
