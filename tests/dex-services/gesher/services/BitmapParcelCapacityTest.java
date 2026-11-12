/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.BadParcelableException;
import android.os.Parcel;
import android.os.SharedMemory;
import java.nio.ByteBuffer;

/** Actual Windows ART/Skia Bitmap and SharedMemory Parcel contracts. */
public final class BitmapParcelCapacityTest {
    private static int checks;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void roundtrip(int width, int height) {
        Bitmap source = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Parcel parcel = Parcel.obtain();
        Bitmap copy = null;
        try {
            source.eraseColor(Color.rgb(51, 102, 153));
            source.setPixel(width - 1, height - 1, Color.RED);
            source.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            copy = Bitmap.CREATOR.createFromParcel(parcel);
            check(copy != null && copy.getWidth() == width && copy.getHeight() == height,
                    "Bitmap dimensions survive Parcel");
            check(copy.getPixel(0, 0) == Color.rgb(51, 102, 153), "First pixel survives Parcel");
            check(copy.getPixel(width - 1, height - 1) == Color.RED, "Last pixel survives Parcel");
        } finally {
            if (copy != null) copy.recycle();
            source.recycle();
            parcel.recycle();
        }
    }

    private static void shortMapping() throws Exception {
        Bitmap source = Bitmap.createBitmap(128, 128, Bitmap.Config.ARGB_8888);
        Parcel original = Parcel.obtain();
        Parcel shortParcel = Parcel.obtain();
        try (SharedMemory small = SharedMemory.create("gesher-bitmap-capacity", 4096)) {
            source.writeToParcel(original, 0);
            original.setDataPosition(0);
            // Preserve the actual writer's pixel format and color-space header.
            for (int i = 0; i < 3; i++) shortParcel.writeInt(original.readInt());
            int hasColorSpace = original.readInt();
            shortParcel.writeInt(hasColorSpace);
            if (hasColorSpace != 0) shortParcel.writeByteArray(original.createByteArray());
            for (int i = 0; i < 4; i++) shortParcel.writeInt(original.readInt());
            shortParcel.writeLong(original.readLong());
            shortParcel.writeLong(original.readLong());
            check(original.readInt() == 1, "Large bitmap uses real SharedMemory");
            int declaredSize = original.readInt();
            check(declaredSize > small.getSize(), "Fixture mapping is shorter than the pixel allocation");
            shortParcel.writeInt(1);
            shortParcel.writeInt(declaredSize);
            small.writeToParcel(shortParcel, 0);
            shortParcel.setDataPosition(0);
            boolean rejected = false;
            Bitmap unexpected = null;
            try { unexpected = Bitmap.CREATOR.createFromParcel(shortParcel); }
            catch (BadParcelableException expected) { rejected = true; }
            finally { if (unexpected != null) unexpected.recycle(); }
            check(rejected, "Declared size cannot enlarge the actual mapping");
            // Closing the reader's duplicate must leave the sender's memory usable.
            ByteBuffer mapped = small.mapReadWrite();
            try {
                mapped.putInt(0, 0x12345678);
                check(mapped.getInt(0) == 0x12345678, "Sender retains its independent SharedMemory");
            } finally { SharedMemory.unmap(mapped); }
        } finally {
            source.recycle();
            original.recycle();
            shortParcel.recycle();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        roundtrip(16, 16);
        roundtrip(128, 128);
        for (int i = 0; i < 4; i++) shortMapping();
        roundtrip(128, 128);
        System.out.println("BitmapParcelCapacityTest all passed: " + checks + " checks");
    }
}
