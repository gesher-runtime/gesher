/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.BadParcelableException;
import android.os.Parcel;
import android.os.SharedMemory;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.nio.ByteBuffer;
import java.util.Arrays;

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

    private static void zeroStride(int dimension) {
        Bitmap source = Bitmap.createBitmap(dimension, dimension, Bitmap.Config.ARGB_8888);
        Parcel parcel = Parcel.obtain();
        Bitmap unexpected = null;
        try {
            source.eraseColor(Color.RED);
            source.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            for (int i = 0; i < 3; ++i) parcel.readInt();
            if (parcel.readInt() != 0) parcel.createByteArray();
            parcel.readInt(); parcel.readInt();
            // SkBitmap.setInfo treats zero as a default stride, but the wire
            // allocation must not be computed as zero for nonempty pixels.
            parcel.writeInt(0);
            parcel.setDataPosition(0);
            boolean rejected = false;
            try { unexpected = Bitmap.CREATOR.createFromParcel(parcel); }
            catch (BadParcelableException expected) { rejected = true; }
            check(rejected, "Zero serialized stride cannot bypass pixel allocation bounds");
            check(source.getPixel(0, 0) == Color.RED, "Malformed recipient leaves sender intact");
        } finally {
            if (unexpected != null) unexpected.recycle();
            parcel.recycle(); source.recycle();
        }
    }

    private static void sharedChain(int width, int height, Bitmap.Config config) throws Exception {
        Bitmap mutable = Bitmap.createBitmap(width, height, config);
        Bitmap current = null;
        try {
            mutable.setDensity(231);
            mutable.eraseColor(Color.argb(128, 51, 102, 153));
            mutable.setPixel(width - 1, height - 1, Color.RED);
            int[] expected = new int[width * height];
            mutable.getPixels(expected, 0, width, 0, 0, width, height);
            current = mutable.asShared();
            check(current != mutable && !current.isMutable(), "asShared makes an independent immutable snapshot");
            check(current.asShared() == current, "Actual native shared storage makes asShared idempotent");
            mutable.eraseColor(Color.GREEN);
            mutable.recycle();
            for (int hop = 0; hop < 4; ++hop) {
                Parcel parcel = Parcel.obtain();
                try {
                    current.writeToParcel(parcel, 0);
                    check(parcel.hasFileDescriptors(), "Both small and large shared Bitmaps retain descriptor transport");
                    // The Parcel already owns its duplicate; the receiver must
                    // not need the original native Bitmap or Java owner.
                    current.recycle(); current = null;
                    parcel.setDataPosition(0);
                    current = Bitmap.CREATOR.createFromParcel(parcel);
                } finally { parcel.recycle(); }
                check(current != null && !current.isMutable() && current.asShared() == current,
                        "Recipient owns an immutable native shared Bitmap");
                check(current.getWidth() == width && current.getHeight() == height
                        && current.getDensity() == 231, "Shared dimensions and density survive each hop");
                int[] actual = new int[width * height];
                current.getPixels(actual, 0, width, 0, 0, width, height);
                check(Arrays.equals(expected, actual), "Every pixel survives sender and Parcel retirement");
                try (SharedMemory memory = current.getSharedMemory()) {
                    check(memory != null && memory.getSize() >= current.getRowBytes() * height,
                            "Bitmap exposes its independently owned actual shared region");
                    ByteBuffer view = memory.mapReadOnly();
                    try {
                        check(view.isReadOnly() && view.capacity() >= current.getRowBytes() * height,
                                "Received region permits a real bounded read-only mapping");
                    } finally { SharedMemory.unmap(view); }
                    ByteBuffer invalid = null;
                    boolean denied = false;
                    try { invalid = memory.mapReadWrite(); }
                    catch (ErrnoException expectedFailure) { denied = expectedFailure.errno == OsConstants.EPERM; }
                    finally { if (invalid != null) SharedMemory.unmap(invalid); }
                    check(denied, "Immutable region rejects future shared writable mappings");
                    denied = false;
                    try { Os.ftruncate(memory.getFileDescriptor(), 1); }
                    catch (ErrnoException expectedFailure) { denied = expectedFailure.errno == OsConstants.EPERM; }
                    check(denied, "Immutable shared region cannot be shrunk under a Bitmap");
                }
                check(current.getPixel(width - 1, height - 1) == expected[expected.length - 1],
                        "Closing an exported SharedMemory duplicate leaves the Bitmap alive");
            }
        } finally {
            if (current != null) current.recycle();
            mutable.recycle();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        roundtrip(16, 16);
        roundtrip(128, 128);
        for (int i = 0; i < 4; i++) shortMapping();
        roundtrip(128, 128);
        zeroStride(16);
        zeroStride(128);
        sharedChain(7, 5, Bitmap.Config.ARGB_8888);
        sharedChain(128, 96, Bitmap.Config.ARGB_8888);
        sharedChain(7, 9, Bitmap.Config.RGB_565);
        sharedChain(3, 7, Bitmap.Config.ALPHA_8);
        sharedChain(9, 7, Bitmap.Config.RGBA_F16);
        System.out.println("BitmapParcelCapacityTest all passed: " + checks + " checks");
    }
}
