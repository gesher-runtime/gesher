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

    private static final int HEADER_BYTES = 4 * Integer.BYTES;
    // The existing 64 MiB image limit plus room for inline text and HTML.
    private static final int MAX_FRAME_BYTES = 72 * 1024 * 1024 + HEADER_BYTES;
    private static final int STATE_BYTES = 3 * Integer.BYTES;
    static final int CAN_READ = 1;
    static final int CAN_WRITE = 2;
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

    static byte[] encode(ClipboardService.Snapshot clipboard) {
        String text = clipboard.text(), html = clipboard.html();
        byte[] image = clipboard.image();
        int textUnits = text == null ? -1 : text.length();
        int htmlUnits = html == null ? -1 : html.length();
        int imageBytes = image == null ? -1 : image.length;
        long size = HEADER_BYTES + payloadSize(textUnits, htmlUnits, imageBytes);
        if (size > MAX_FRAME_BYTES || imageBytes > ClipboardService.MAX_IMAGE_BYTES) return null;
        ByteBuffer frame = ByteBuffer.allocate((int)size);
        frame.putInt(clipboard.sequence()).putInt(textUnits).putInt(htmlUnits).putInt(imageBytes);
        writeText(frame, text);
        writeText(frame, html);
        if (image != null) frame.put(image);
        return frame.array();
    }

    static ClipboardService.Snapshot decode(byte[] data) {
        if (data == null || data.length < HEADER_BYTES || data.length > MAX_FRAME_BYTES) return null;
        ByteBuffer frame = ByteBuffer.wrap(data);
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

    private static int sequenceFrom(byte[] data) {
        return data == null || data.length != Integer.BYTES ? 0 : ByteBuffer.wrap(data).getInt();
    }

    private State state() {
        byte[] data = Zygote.nativeCohostClipboardExchange(STATE, null);
        if (data == null || data.length != STATE_BYTES) return new State(0, -1, 0);
        ByteBuffer frame = ByteBuffer.wrap(data);
        return new State(frame.getInt(), frame.getInt(), frame.getInt());
    }

    @Override public boolean available() { return true; }
    @Override public synchronized boolean authorized(int op) {
        int permission = switch (op) {
            case AppOpsManager.OP_READ_CLIPBOARD -> CAN_READ;
            case AppOpsManager.OP_WRITE_CLIPBOARD -> CAN_WRITE;
            default -> 0;
        };
        return (state().permissions() & permission) != 0;
    }
    @Override public synchronized int sessionLockState() { return state().locked(); }
    @Override public synchronized ClipboardService.Sample sample(int previousSequence) {
        State state = state();
        if (state.sequence() == 0 || state.sequence() == previousSequence) return null;
        if (state.locked() != 0) return new ClipboardService.Sample(state.sequence(), null);
        ClipboardService.Snapshot clip = decode(Zygote.nativeCohostClipboardExchange(READ, null));
        return clip == null ? null : new ClipboardService.Sample(clip.sequence(), clip);
    }

    @Override public synchronized int write(String text, String html, byte[] image) {
        byte[] data = encode(new ClipboardService.Snapshot(0, text, html, image));
        return data == null ? 0 : sequenceFrom(Zygote.nativeCohostClipboardExchange(WRITE, data));
    }

    @Override public synchronized int clear() {
        return sequenceFrom(Zygote.nativeCohostClipboardExchange(CLEAR, null));
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
