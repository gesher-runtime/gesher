/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.graphics.Point;
import android.media.MediaMetadataRetriever;
import android.os.CancellationSignal;
import android.os.SharedMemory;
import android.system.ErrnoException;
import android.system.OsConstants;
import android.util.Size;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.ByteBuffer;

/** Renders a disposable thumbnail of an already-authorized MediaStore item. */
final class MediaStoreThumbnail {
    private static final int MAX_SIDE = 1024;

    private MediaStoreThumbnail() {}

    static AssetFileDescriptor open(File file, boolean video, String mimeType, Point requested,
            CancellationSignal signal) throws FileNotFoundException {
        if (requested == null || requested.x <= 0 || requested.y <= 0) {
            throw new IllegalArgumentException("Thumbnail size must have positive dimensions");
        }
        Bitmap.CompressFormat format;
        if ("image/jpeg".equalsIgnoreCase(mimeType)) {
            format = Bitmap.CompressFormat.JPEG;
        } else if ("image/png".equalsIgnoreCase(mimeType) || "image/*".equalsIgnoreCase(mimeType)) {
            format = Bitmap.CompressFormat.PNG;
        } else {
            throw new FileNotFoundException("Unsupported thumbnail format: " + mimeType);
        }

        Size bounds = new Size(Math.min(requested.x, MAX_SIDE), Math.min(requested.y, MAX_SIDE));
        checkCancellation(signal);
        try {
            Bitmap bitmap = video ? videoFrame(file, bounds) : image(file, bounds, signal);
            if (bitmap == null) throw new IOException("No thumbnail pixels available");
            try {
                checkCancellation(signal);
                ByteArrayOutputStream stream = new ByteArrayOutputStream();
                if (!bitmap.compress(format, 85, stream)) {
                    throw new IOException("Could not encode media thumbnail");
                }
                return export(stream.toByteArray(), signal);
            } finally {
                bitmap.recycle();
            }
        } catch (IOException | ErrnoException failure) {
            FileNotFoundException error = new FileNotFoundException("Cannot load media thumbnail");
            error.initCause(failure);
            throw error;
        }
    }

    private static Bitmap image(File file, Size bounds, CancellationSignal signal)
            throws IOException {
        return ImageDecoder.decodeBitmap(ImageDecoder.createSource(file), (decoder, info, source) -> {
            checkCancellation(signal);
            Size original = info.getSize();
            Size size = fit(original.getWidth(), original.getHeight(), bounds.getWidth(), bounds.getHeight());
            decoder.setTargetSize(size.getWidth(), size.getHeight());
            decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
            decoder.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.SRGB));
        });
    }

    private static Bitmap videoFrame(File file, Size bounds) throws IOException {
        try (MediaMetadataRetriever retriever = new MediaMetadataRetriever()) {
            retriever.setDataSource(file.getAbsolutePath());
            return retriever.getScaledFrameAtTime(-1, MediaMetadataRetriever.OPTION_CLOSEST_SYNC,
                    bounds.getWidth(), bounds.getHeight());
        } catch (RuntimeException failure) {
            throw new IOException("Could not retrieve a video frame", failure);
        }
    }

    private static AssetFileDescriptor export(byte[] bytes, CancellationSignal signal)
            throws IOException, ErrnoException {
        checkCancellation(signal);
        // A seekable snapshot contains only the encoded bitmap and has no persistent path.
        try (SharedMemory memory = SharedMemory.create("media-thumbnail", bytes.length)) {
            ByteBuffer buffer = memory.mapReadWrite();
            try {
                buffer.put(bytes);
            } finally {
                SharedMemory.unmap(buffer);
            }
            if (!memory.setProtect(OsConstants.PROT_READ)) {
                throw new IOException("Could not protect media thumbnail");
            }
            checkCancellation(signal);
            return new AssetFileDescriptor(memory.getFdDup(), 0, bytes.length);
        }
    }

    private static Size fit(int width, int height, int maxWidth, int maxHeight) {
        double scale = Math.min(1.0, Math.min((double) maxWidth / width, (double) maxHeight / height));
        return new Size(Math.max(1, (int) Math.round(width * scale)),
                Math.max(1, (int) Math.round(height * scale)));
    }

    private static void checkCancellation(CancellationSignal signal) {
        if (signal != null) signal.throwIfCanceled();
    }
}
