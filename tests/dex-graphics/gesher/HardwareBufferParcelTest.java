/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Exercises parcel descriptor ownership with pixels published by an Android Surface. */
public final class HardwareBufferParcelTest {
    private static void check(boolean passed, String operation) {
        if (!passed) throw new AssertionError(operation);
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 0 || !"--android".equals(args[0])) {
            System.loadLibrary("android_runtime");
        }
        HandlerThread callbacks = new HandlerThread("hardware-buffer-parcel");
        callbacks.start();
        try (ImageReader reader = new ImageReader.Builder(18, 10).setMaxImages(2)
                .setImageFormat(PixelFormat.RGBA_8888)
                .setUsage(HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE).build()) {
            CountDownLatch available = new CountDownLatch(1);
            reader.setOnImageAvailableListener(ignored -> available.countDown(),
                    new Handler(callbacks.getLooper()));
            Canvas canvas = reader.getSurface().lockCanvas(null);
            canvas.drawColor(Color.RED);
            canvas.clipRect(0, 0, 9, 10);
            canvas.drawColor(Color.BLUE);
            reader.getSurface().unlockCanvasAndPost(canvas);
            check(available.await(5, TimeUnit.SECONDS), "published ImageReader frame");
            try (Image image = reader.acquireNextImage()) {
                check(image != null, "acquired ImageReader frame");
                roundTrip(image);
            }
        } finally {
            callbacks.quitSafely();
            callbacks.join(5000);
            check(!callbacks.isAlive(), "finished callback thread");
        }
        System.out.println("HardwareBufferParcelTest all passed");
    }

    private static void roundTrip(Image image) {
        Parcel sourceParcel = Parcel.obtain();
        Parcel receivingParcel = Parcel.obtain();
        try {
            final long id;
            final long usage;
            try (HardwareBuffer source = image.getHardwareBuffer()) {
                check(source != null, "reader HardwareBuffer");
                id = source.getId();
                usage = source.getUsage();
                check((source.describeContents() & Parcelable.CONTENTS_FILE_DESCRIPTOR) != 0,
                        "descriptor advertised");
                sourceParcel.writeInt(42);
                source.writeToParcel(sourceParcel, 0);
                sourceParcel.writeInt(99);
                check(sourceParcel.hasFileDescriptors(), "parcel contains backing descriptor");
                receivingParcel.appendFrom(sourceParcel, 0, sourceParcel.dataSize());
            }
            sourceParcel.recycle();
            sourceParcel = null;

            receivingParcel.setDataPosition(0);
            check(receivingParcel.readInt() == 42, "field before HardwareBuffer");
            try (HardwareBuffer restored = HardwareBuffer.CREATOR.createFromParcel(receivingParcel)) {
                check(restored != null, "read HardwareBuffer after source owners close");
                check(receivingParcel.readInt() == 99 && receivingParcel.dataAvail() == 0,
                        "field after HardwareBuffer");
                receivingParcel.recycle();
                receivingParcel = null;
                check(restored.getId() == id && restored.getUsage() == usage,
                        "same backing identity and usage");
                check(restored.getWidth() == 18 && restored.getHeight() == 10
                                && restored.getFormat() == HardwareBuffer.RGBA_8888
                                && restored.getLayers() == 1, "same buffer description");
                checkPixels(restored);
            }
        } finally {
            if (sourceParcel != null) sourceParcel.recycle();
            if (receivingParcel != null) receivingParcel.recycle();
        }
    }

    private static void checkPixels(HardwareBuffer restored) {
        Bitmap wrapped = Bitmap.wrapHardwareBuffer(restored, null);
        check(wrapped != null, "wrapped restored buffer");
        try {
            Bitmap cpu = wrapped.copy(Bitmap.Config.ARGB_8888, false);
            check(cpu != null, "made restored pixels readable for comparison");
            try {
                check(cpu.getPixel(2, 6) == Color.BLUE && cpu.getPixel(15, 6) == Color.RED,
                        "restored producer pixels preserve color and row stride");
            } finally {
                cpu.recycle();
            }
        } finally {
            wrapped.recycle();
        }
    }
}
