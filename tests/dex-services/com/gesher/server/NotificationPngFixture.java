/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;

/** Test-only file/content oracle; the production toast receives this same PNG path. */
final class NotificationPngFixture {
    private static final byte[] SIGNATURE = {(byte)0x89, 'P', 'N', 'G', 13, 10, 26, 10};

    private NotificationPngFixture() { }

    static final class Saved {
        final byte[] bytes;
        final String path, sha256;
        final int width, height;

        Saved(File file) throws Exception {
            path = file.getCanonicalPath();
            if (!file.isFile()) throw new AssertionError("Saved notification PNG is missing: " + path);
            bytes = Files.readAllBytes(file.toPath());
            sha256 = hex(MessageDigest.getInstance("SHA-256").digest(bytes));
            System.out.println("NOTIFICATION_PNG_FILE path=" + path + " bytes=" + bytes.length
                    + " sha256=" + sha256 + " header=" + hex(Arrays.copyOf(bytes, Math.min(33, bytes.length))));
            if (bytes.length < 33 || !Arrays.equals(SIGNATURE, Arrays.copyOf(bytes, 8))) {
                throw new AssertionError("Saved notification file has no complete PNG header: " + path);
            }
            ByteBuffer header = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN);
            if (header.getInt(8) != 13 || header.getInt(12) != 0x49484452) {
                throw new AssertionError("Saved notification PNG has no initial IHDR: " + path);
            }
            width = header.getInt(16); height = header.getInt(20);
            if (width <= 0 || height <= 0) throw new AssertionError("Invalid saved PNG dimensions: " + path);
        }

        Bitmap decode() {
            // Android 16's host nativeDecodeFileDescriptor returns null under
            // _WIN32. decodeByteArray uses the real Skia decoder on the exact
            // bytes read above, without that unrelated descriptor API gap.
            Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
            if (bitmap == null) throw new AssertionError("Native PNG byte decoder returned null: " + path
                    + " bytes=" + bytes.length + " ihdr=" + width + "x" + height + " sha256=" + sha256);
            int actualWidth = bitmap.getWidth(), actualHeight = bitmap.getHeight();
            System.out.println("NOTIFICATION_PNG_DECODE path=" + path + " ihdr=" + width + "x" + height
                    + " decoded=" + actualWidth + "x" + actualHeight);
            if (actualWidth != width || actualHeight != height) {
                bitmap.recycle();
                throw new AssertionError("Native PNG dimensions differ from saved IHDR: " + path);
            }
            return bitmap;
        }

        String receipt() { return path + "\n" + bytes.length + "\n" + sha256 + "\n"; }
    }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(Character.forDigit((value >>> 4) & 15, 16))
                .append(Character.forDigit(value & 15, 16));
        return result.toString();
    }
}
