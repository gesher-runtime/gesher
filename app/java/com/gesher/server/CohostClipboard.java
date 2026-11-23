/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;

import java.nio.ByteBuffer;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/** Transfers the existing clipboard formats over one host-admitted cohost scope. */
final class CohostClipboard implements ClipboardService.Host {
    static final int STATE = 6;
    static final int READ = 7;
    static final int WRITE = 8;
    static final int CLEAR = 9;
    static final int METADATA = 10;
    static final int CALLBACK = 11;

    private static final int HEADER_BYTES = 4 * Integer.BYTES;
    // The existing 64 MiB image limit plus room for inline text and HTML.
    private static final int MAX_FRAME_BYTES = 72 * 1024 * 1024 + HEADER_BYTES;
    private static final int STATE_BYTES = 3 * Integer.BYTES;
    private static final int MAX_ATTRIBUTION_TAG = 50;
    private static final char NO_ATTRIBUTION_TAG = '\uffff';
    static final int CAN_READ = 1;
    static final int CAN_WRITE = 2;
    static final int PLAIN_PRESENT = 1;
    static final int HTML_PRESENT = 2;
    static final int IMAGE_PRESENT = 4;
    static final int NONEMPTY_TEXT = 8;
    private final BooleanSupplier mHasListeners;
    private ScheduledExecutorService mEvents;

    CohostClipboard(BooleanSupplier hasListeners) {
        mHasListeners = hasListeners;
    }

    record State(int sequence, int locked, int permissions) {
        byte[] encode() {
            return ByteBuffer.allocate(STATE_BYTES).putInt(sequence).putInt(locked).putInt(permissions)
                    .array();
        }
    }

    record Metadata(int sequence, int formats, long timestamp) {
        byte[] encode() {
            return ByteBuffer.allocate(2 * Integer.BYTES + Long.BYTES)
                    .putInt(sequence).putInt(formats).putLong(timestamp).array();
        }

        boolean present() { return (formats & (PLAIN_PRESENT | HTML_PRESENT | IMAGE_PRESENT)) != 0; }
        boolean hasText() { return (formats & NONEMPTY_TEXT) != 0; }
    }

    static byte[] encode(ClipboardService.Snapshot clipboard) {
        return encode(clipboard, null, false);
    }

    private static byte[] encode(ClipboardService.Snapshot clipboard, String tag, boolean request) {
        String text = clipboard.text(), html = clipboard.html();
        byte[] image = clipboard.image();
        int textUnits = text == null ? -1 : text.length();
        int htmlUnits = html == null ? -1 : html.length();
        int imageBytes = image == null ? -1 : image.length;
        long size = HEADER_BYTES + payloadSize(textUnits, htmlUnits, imageBytes);
        if (size > MAX_FRAME_BYTES || imageBytes > ClipboardService.MAX_IMAGE_BYTES) return null;
        ByteBuffer frame = request ? request(tag, (int)size) : ByteBuffer.allocate((int)size);
        frame.putInt(clipboard.sequence()).putInt(textUnits).putInt(htmlUnits).putInt(imageBytes);
        writeText(frame, text);
        writeText(frame, html);
        if (image != null) frame.put(image);
        return frame.array();
    }

    static ClipboardService.Snapshot decode(byte[] data) {
        return data == null ? null : decode(ByteBuffer.wrap(data));
    }

    static ClipboardService.Snapshot decode(ByteBuffer frame) {
        if (frame.remaining() < HEADER_BYTES || frame.remaining() > MAX_FRAME_BYTES) return null;
        int sequence = frame.getInt();
        int textUnits = frame.getInt(), htmlUnits = frame.getInt(), imageBytes = frame.getInt();
        if (textUnits < -1 || htmlUnits < -1
                || imageBytes < -1 || imageBytes > ClipboardService.MAX_IMAGE_BYTES
                || frame.remaining() != payloadSize(textUnits, htmlUnits, imageBytes)) {
            return null;
        }
        String text = readText(frame, textUnits), html = readText(frame, htmlUnits);
        byte[] image = null;
        if (imageBytes >= 0) {
            image = new byte[imageBytes];
            frame.get(image);
        }
        return new ClipboardService.Snapshot(sequence, text, html, image);
    }

    private static long payloadSize(int textUnits, int htmlUnits, int imageBytes) {
        return Character.BYTES * ((long)Math.max(textUnits, 0) + Math.max(htmlUnits, 0))
                + Math.max(imageBytes, 0);
    }

    private static void writeText(ByteBuffer frame, String text) {
        if (text == null) return;
        for (int i = 0; i < text.length(); i++) frame.putChar(text.charAt(i));
    }

    private static String readText(ByteBuffer frame, int units) {
        if (units < 0) return null;
        char[] chars = new char[units];
        frame.asCharBuffer().get(chars);
        frame.position(frame.position() + Character.BYTES * units);
        return new String(chars);
    }

    static byte[] receipt(int sequence) {
        return ByteBuffer.allocate(Integer.BYTES).putInt(sequence).array();
    }

    private static ByteBuffer request(String tag, int payloadBytes) {
        if (tag != null && tag.length() > MAX_ATTRIBUTION_TAG) tag = null;
        ByteBuffer frame = ByteBuffer.allocate(Character.BYTES + payloadBytes
                + (tag == null ? 0 : Character.BYTES * tag.length()));
        frame.putChar(tag == null ? NO_ATTRIBUTION_TAG : (char)tag.length());
        writeText(frame, tag);
        return frame;
    }

    static String readAttribution(ByteBuffer frame) {
        int units = frame.getChar();
        if (units == NO_ATTRIBUTION_TAG) return null;
        if (units > MAX_ATTRIBUTION_TAG || frame.remaining() < Character.BYTES * units) {
            throw new IllegalArgumentException("Invalid clipboard attribution tag frame");
        }
        return readText(frame, units);
    }

    private static int sequenceFrom(byte[] data) {
        return data == null || data.length != Integer.BYTES ? 0 : ByteBuffer.wrap(data).getInt();
    }

    private State state(String tag) {
        byte[] data = Zygote.nativeCohostClipboardExchange(STATE, request(tag, 0).array());
        if (data == null || data.length != STATE_BYTES) return new State(0, -1, 0);
        ByteBuffer frame = ByteBuffer.wrap(data);
        return new State(frame.getInt(), frame.getInt(), frame.getInt());
    }

    @Override public boolean available() { return true; }
    synchronized boolean authorized(int op, String tag) {
        int permission = switch (op) {
            case AppOpsManager.OP_READ_CLIPBOARD -> CAN_READ;
            case AppOpsManager.OP_WRITE_CLIPBOARD -> CAN_WRITE;
            default -> 0;
        };
        return (state(tag).permissions() & permission) != 0;
    }
    @Override public synchronized int sessionLockState() { return state(null).locked(); }
    @Override public synchronized ClipboardService.Sample sample(int previousSequence) {
        State state = state(null);
        if (state.sequence() == 0 || state.sequence() == previousSequence) return null;
        return new ClipboardService.Sample(state.sequence(), null);
    }

    synchronized Metadata metadata(int cachedSequence, String tag) {
        byte[] data = Zygote.nativeCohostClipboardExchange(METADATA,
                request(tag, Integer.BYTES).putInt(cachedSequence).array());
        if (data == null || data.length != 2 * Integer.BYTES + Long.BYTES) return null;
        ByteBuffer frame = ByteBuffer.wrap(data);
        return new Metadata(frame.getInt(), frame.getInt(), frame.getLong());
    }

    synchronized ClipboardService.Sample read(int cachedSequence, String tag) {
        byte[] data = Zygote.nativeCohostClipboardExchange(READ,
                request(tag, Integer.BYTES).putInt(cachedSequence).array());
        if (cachedSequence != 0 && sequenceFrom(data) == cachedSequence) {
            return new ClipboardService.Sample(cachedSequence, null);
        }
        ClipboardService.Snapshot clip = decode(data);
        return clip == null ? null : new ClipboardService.Sample(clip.sequence(), clip);
    }

    synchronized boolean noteCallback(String tag) {
        return sequenceFrom(Zygote.nativeCohostClipboardExchange(CALLBACK, request(tag, 0).array())) == 1;
    }

    @Override public synchronized int write(String text, String html, byte[] image) {
        return write(text, html, image, null);
    }

    synchronized int write(String text, String html, byte[] image, String tag) {
        byte[] data = encode(new ClipboardService.Snapshot(0, text, html, image), tag, true);
        return data == null ? 0 : sequenceFrom(Zygote.nativeCohostClipboardExchange(WRITE, data));
    }

    @Override public synchronized int clear() {
        return clear(null);
    }

    synchronized int clear(String tag) {
        return sequenceFrom(Zygote.nativeCohostClipboardExchange(CLEAR, request(tag, 0).array()));
    }

    @Override public synchronized void addListener(WindowsHost.Listener listener) {
        if (mEvents != null) return;
        mEvents = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "cohost-clipboard");
            thread.setDaemon(true);
            return thread;
        });
        // A protected window cannot rely on receiving desktop clipboard broadcasts.
        // Only poll while Android callbacks are registered; reads refresh on demand.
        mEvents.scheduleWithFixedDelay(() -> {
            if (mHasListeners.getAsBoolean()) {
                try {
                    listener.onWindowsEvents(WindowsHost.EVENT_CLIPBOARD);
                } catch (RuntimeException unavailable) {
                    SystemServer.log("cohost clipboard event: " + unavailable.getClass().getSimpleName());
                }
            }
        }, 250, 250, TimeUnit.MILLISECONDS);
    }

    @Override public synchronized void removeListener(WindowsHost.Listener listener) {
        if (mEvents == null) return;
        mEvents.shutdownNow();
        mEvents = null;
    }
}
