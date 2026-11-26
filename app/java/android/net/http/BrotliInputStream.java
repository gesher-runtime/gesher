// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.Cleaner;
import java.util.Objects;

/** Incremental RFC 7932 decoding, backed by the pinned native AOSP provider. */
final class BrotliInputStream extends InputStream {
    private static final int CHUNK = 32768;
    private static final Cleaner CLEANER = Cleaner.create();
    static { System.loadLibrary("gesher_brotli"); }
    static void ensureAvailable() { /* Class initialization loads the real provider. */ }
    private static native long nativeCreate();
    private static native long nativeProcess(long handle, byte[] input, int position, int remaining,
                                             byte[] output, int offset, int length);
    private static native void nativeDestroy(long handle);

    private static final class Owner implements Runnable {
        private long handle = nativeCreate();
        synchronized long process(byte[] in, int position, int remaining,
                                  byte[] out, int offset, int length) throws IOException {
            if (handle == 0) throw new IOException("Brotli decoder is closed");
            return nativeProcess(handle, in, position, remaining, out, offset, length);
        }
        @Override public synchronized void run() {
            if (handle != 0) { nativeDestroy(handle); handle = 0; }
        }
    }

    private final InputStream source;
    private final Owner owner;
    private final Cleaner.Cleanable cleanable;
    private final Object reader = new Object();
    private final byte[] compressed = new byte[CHUNK];
    private final byte[] one = new byte[1];
    private int position, remaining;
    private boolean needInput = true, decodedEnd, eof;
    private volatile boolean closed;

    BrotliInputStream(InputStream source) {
        this.source = Objects.requireNonNull(source);
        Owner acquired = new Owner();
        try { cleanable = CLEANER.register(this, acquired); }
        catch (Throwable failure) { acquired.run(); throw failure; }
        owner = acquired;
        // No input is read here. Header callbacks must run before a duplex peer
        // can be expected to send any body bytes.
    }

    @Override public int read() throws IOException {
        synchronized (reader) {
            int count = read(one, 0, 1);
            return count < 0 ? -1 : one[0] & 255;
        }
    }
    @Override public int read(byte[] bytes, int offset, int count) throws IOException {
        Objects.checkFromIndexSize(offset, count, bytes.length);
        if (count == 0) return 0;
        synchronized (reader) {
            if (closed) throw new IOException("Stream is closed");
            if (eof) return -1;
            try {
                for (;;) {
                    if (closed) throw new IOException("Stream is closed");
                    if (decodedEnd) {
                        // Chromium's Brotli filter discards trailing encoded
                        // bytes. Drain the real body to its framing boundary so
                        // the provider can commit its cache entry/reuse its socket.
                        if (source.read(compressed) >= 0) continue;
                        eof = true;
                        return -1;
                    }
                    if (needInput && remaining == 0) {
                        remaining = source.read(compressed);
                        position = 0;
                        if (remaining < 0) throw new IOException("Truncated Brotli response body");
                        if (remaining == 0) continue;
                        needInput = false;
                    }
                    long result = owner.process(compressed, position, remaining,
                                                bytes, offset, Math.min(count, CHUNK));
                    int consumed = (int) (result >>> 32);
                    int produced = (int) ((result & 0xffffffffL) >>> 2);
                    int status = (int) (result & 3);
                    position += consumed;
                    remaining -= consumed;
                    if (status == 1) {
                        decodedEnd = true;
                        remaining = 0;
                        cleanable.clean();
                    } else if (status == 2) {
                        needInput = true;
                    }
                    if (produced > 0) return produced;
                }
            } catch (IOException | RuntimeException | Error failure) {
                cleanable.clean();
                try { close(); } catch (IOException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        }
    }
    @Override public void close() throws IOException {
        synchronized (owner) {
            if (closed) return;
            closed = true;
        }
        // Do not hold the reader/owner monitor while closing the upstream: a
        // concurrent blocked socket read must be cancelable.
        try { source.close(); }
        finally { cleanable.clean(); }
    }
}
