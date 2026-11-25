/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.Application;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Point;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.OperationCanceledException;
import android.provider.MediaStore;
import android.util.Size;

import java.io.InputStream;
import java.io.OutputStream;
import java.util.Base64;
import java.util.UUID;

/** Real MediaStore index, decoder and descriptor on the native Android runtime. */
public final class MediaStoreThumbnailContractTest {
    // Two 64x32 blue H.264 frames. Generated with FFmpeg's color source at 2 fps,
    // yuv420p, no audio, faststart, and encoder SEI removed.
    private static final byte[] VIDEO = Base64.getMimeDecoder().decode("""
            AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMFbW9vdgAAAGxtdmhkAAAAAAAAAAAA
            AAAAAAAD6AAAA+gAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA
            AABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAlR0cmFrAAAAXHRraGQAAAADAAAA
            AAAAAAAAAAABAAAAAAAAA+gAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAA
            AAAAAAAAAABAAAAAAEAAAAAgAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAPoAAAAAAABAAAA
            AAHMbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAQABVxAAAAAAALWhkbHIAAAAAAAAAAHZp
            ZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABd21pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAA
            ACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAATdzdGJsAAAAt3N0c2QAAAAAAAAA
            AQAAAKdhdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAEAAIABIAAAASAAAAAAAAAABDExhdmMg
            bGlieDI2NAAAAAAAAAAAAAAAAAAAAAAAAAAAGP//AAAALWF2Y0MBQsAK/+EAFmdCwAraEWwEQAAA
            AwBAAAADAQPEiagBAARozg/IAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAAAATgAAAAAAAAA
            GHN0dHMAAAAAAAAAAQAAAAIAACAAAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAAcc3RzYwAAAAAAAAAB
            AAAAAQAAAAIAAAABAAAAHHN0c3oAAAAAAAAAAAAAAAIAAAAdAAAACgAAABRzdGNvAAAAAAAAAAEA
            AAM1AAAAPXVkdGEAAAA1bWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAA
            AAAIaWxzdAAAAAhmcmVlAAAAL21kYXQAAAAZZYiEOhGKAAIhccAAQzI4AAgcycnJ11114AAAAAZB
            miASoTA=
            """);

    public static void main(String[] args) throws Exception {
        MediaTestBootstrap.initialize("MediaStoreThumbnailContractTest");
        Application app = (Application) Class.forName("android.app.ActivityThread")
                .getMethod("currentApplication").invoke(null);
        ContentResolver resolver = app.getContentResolver();
        checkImage(resolver);
        checkVideo(resolver);
        System.out.println("MediaStoreThumbnailContractTest all passed: image, video, cancellation");
        System.exit(0);
    }

    private static void checkImage(ContentResolver resolver) throws Exception {
        Uri uri = insert(resolver, MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                ".png", "image/png", "Pictures/");
        try {
            writeImage(resolver, uri, Color.RED);
            checkImage(resolver.loadThumbnail(uri, new Size(44, 36), null), 44, 22, Color.RED);
            // The second request sees the new source pixels without stale output.
            writeImage(resolver, uri, Color.GREEN);
            checkImage(resolver.loadThumbnail(uri, new Size(1000, 1000), null),
                    320, 160, Color.GREEN);

            Bundle opts = new Bundle();
            opts.putParcelable(ContentResolver.EXTRA_SIZE, new Point(44, 36));
            try (AssetFileDescriptor asset = resolver.openTypedAssetFileDescriptor(
                    uri, "image/jpeg", opts, null);
                    InputStream input = asset.createInputStream()) {
                require(input.read() == 0xff && input.read() == 0xd8,
                        "a typed JPEG request receives JPEG bytes");
            }
            CancellationSignal cancelled = new CancellationSignal();
            cancelled.cancel();
            try {
                resolver.loadThumbnail(uri, new Size(44, 36), cancelled);
                throw new AssertionError("pre-cancelled thumbnail returned a bitmap");
            } catch (OperationCanceledException expected) {
                // The client cancellation contract still applies.
            }
        } finally {
            resolver.delete(uri, null, null);
        }
    }

    private static void checkVideo(ContentResolver resolver) throws Exception {
        Uri uri = insert(resolver, MediaStore.Video.Media.EXTERNAL_CONTENT_URI,
                ".mp4", "video/mp4", "Movies/");
        try {
            try (OutputStream output = resolver.openOutputStream(uri, "w")) {
                output.write(VIDEO);
            }
            Bitmap thumbnail = resolver.loadThumbnail(uri, new Size(32, 32), null);
            try {
                require(thumbnail.getWidth() == 32 && thumbnail.getHeight() == 16,
                        "video thumbnail is fitted to the bounds");
                int pixel = thumbnail.getPixel(16, 8);
                require(Color.blue(pixel) > Color.red(pixel) + 100, "video thumbnail shows the source frame");
            } finally {
                thumbnail.recycle();
            }
        } finally {
            resolver.delete(uri, null, null);
        }
    }

    private static Uri insert(ContentResolver resolver, Uri collection, String extension,
            String mimeType, String relativePath) {
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, "thumbnail-" + UUID.randomUUID() + extension);
        values.put(MediaStore.MediaColumns.MIME_TYPE, mimeType);
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath);
        Uri uri = resolver.insert(collection, values);
        require(uri != null, "MediaStore inserted a source item");
        return uri;
    }

    private static void writeImage(ContentResolver resolver, Uri uri, int color) throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(320, 160, Bitmap.Config.ARGB_8888);
        try (OutputStream output = resolver.openOutputStream(uri, "wt")) {
            bitmap.eraseColor(color);
            require(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output), "wrote a source PNG");
        } finally {
            bitmap.recycle();
        }
    }

    private static void checkImage(Bitmap bitmap, int width, int height, int color) {
        try {
            require(bitmap.getWidth() == width && bitmap.getHeight() == height,
                    "image thumbnail fits without upscaling");
            require(bitmap.getPixel(width / 2, height / 2) == color, "thumbnail contains source pixels");
        } finally {
            bitmap.recycle();
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
