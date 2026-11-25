/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.graphics.Point;
import android.media.MediaMetadataRetriever;
import android.os.CancellationSignal;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Size;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

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
                return export(bitmap, format, signal);
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

    private static AssetFileDescriptor export(Bitmap bitmap, Bitmap.CompressFormat format,
            CancellationSignal signal)
            throws IOException, ErrnoException {
        checkCancellation(signal);
        Path directory = new File(Environment.getDataDirectory(), "system/media").toPath();
        Files.createDirectories(directory);
        Path snapshot = Files.createTempFile(directory, "thumbnail-", ".tmp");
        ParcelFileDescriptor input = null;
        boolean unlinked = false;
        try {
            try (FileOutputStream output = new FileOutputStream(snapshot.toFile())) {
                // Use a separate read-only file open. Binder duplicates that handle with
                // its original rights. Remove the name before writing any image data.
                input = ParcelFileDescriptor.open(snapshot.toFile(), ParcelFileDescriptor.MODE_READ_ONLY);
                Os.unlink(snapshot.toString());
                unlinked = true;
                checkCancellation(signal);
                if (!bitmap.compress(format, 85, output)) {
                    throw new IOException("Could not encode media thumbnail");
                }
            }
            checkCancellation(signal);
            AssetFileDescriptor asset = new AssetFileDescriptor(input, 0, AssetFileDescriptor.UNKNOWN_LENGTH);
            input = null; // AssetFileDescriptor owns the handle now.
            return asset;
        } finally {
            try {
                if (input != null) input.close();
            } finally {
                if (!unlinked) Files.deleteIfExists(snapshot);
            }
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
