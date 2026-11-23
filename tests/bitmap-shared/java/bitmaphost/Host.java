// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package bitmaphost;
import android.graphics.Bitmap;
import android.os.BadParcelableException;
import android.os.Parcel;
import android.os.SharedMemory;
import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class Host {
    public static native Bitmap make(int width, int height, int padding, boolean immutable, boolean color);
    public static native Bitmap shared(long handle);
    public static native boolean isShared(long handle);
    public static native boolean immutable(long handle);
    public static native void recycle(long handle);
    public static native long checksum(long handle);
    public static native void change(long handle, int value);
    public static native int ashmemFd(long handle);
    public static native long sourceId(long handle);
    public static native long bitmapId(long handle);
    public static native int rowBytes(long handle);
    public static native boolean hasColor(long handle);
    public static native FileDescriptor createFd(int size);
    public static native FileDescriptor dupFd(FileDescriptor descriptor);
    public static native void closeFd(FileDescriptor descriptor);
    public static native ByteBuffer mapFd(FileDescriptor descriptor, boolean writable);
    public static native void unmap(ByteBuffer buffer);
    public static native long[] stats();
    public static native void fail(int point);
    public static native void reset();
    private static int checks;

    private static synchronized void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static void retired(long[] before, String message) {
        long[] after = stats();
        check(before[0] == after[0] && before[1] == after[1], message + " fd/map retirement");
    }
    private static Bitmap receive(Parcel parcel) {
        parcel.rewind(); return Bitmap.CREATOR.createFromParcel(parcel);
    }
    private static void failure(Runnable operation, Class<? extends Throwable> type, String message) {
        try { operation.run(); }
        catch (Throwable expected) {
            if (!type.isInstance(expected)) throw new AssertionError(message, expected);
            checks++; return;
        }
        throw new AssertionError(message + " unexpectedly succeeded");
    }

    private static void ownership(int width, int height, int padding) {
        long[] before = stats();
        Bitmap mutable = make(width, height, padding, false, true);
        mutable.density = 231;
        long pixels = mutable.checksum();
        Bitmap source = mutable.asShared();
        try {
            check(source != mutable && !source.isMutable() && source.isShared(), "asShared creates actual immutable shared storage");
            check(source.asShared() == source, "asShared is idempotent");
            check(source.checksum() == pixels && source.density == 231, "shared copy preserves pixels and density");
            mutable.change(0xffee9977);
            check(source.checksum() == pixels, "mutable source cannot change shared snapshot");
            mutable.recycle();
            for (int hop = 0; hop < 5; ++hop) {
                Bitmap recipient;
                try (Parcel parcel = new Parcel()) {
                    long id = bitmapId(source.mNativePtr);
                    int senderFd = ashmemFd(source.mNativePtr);
                    reset(); source.writeToParcel(parcel, 0);
                    check(parcel.hasFileDescriptors(), "small and large shared storage forwards descriptors");
                    recipient = receive(parcel);
                    long[] work = stats();
                    check(work[2] == 0 && work[4] == 0 && work[6] == 0, "forward/receive allocates no heap pixels, copy or memfd");
                    check(recipient.isShared() && !recipient.isMutable() && recipient.asShared() == recipient, "recipient retains native shared identity");
                    check(ashmemFd(recipient.mNativePtr) >= 0 && ashmemFd(recipient.mNativePtr) != senderFd, "recipient owns independent descriptor");
                    check(sourceId(recipient.mNativePtr) == id, "source identity follows actual sender");
                    check(rowBytes(recipient.mNativePtr) == width * 4 + padding && hasColor(recipient.mNativePtr), "stride and serialized color space survive");
                    check(recipient.density == 231, "density survives each hop");
                    source.recycle();
                }
                source = recipient;
                check(source.checksum() == pixels, "recipient survives sender recycle and parcel close");
            }
        } finally { mutable.recycle(); source.recycle(); }
        retired(before, "shared chain");
    }

    private static void inlineAndSnapshots() {
        long[] before = stats();
        for (int size : new int[] {8, 96}) {
            try (Bitmap mutable = make(size, size, 8, false, false)) {
                long original = mutable.checksum();
                try (Parcel p = new Parcel()) {
                    mutable.writeToParcel(p, 0);
                    check(p.hasFileDescriptors() == (size > 8), "ordinary small inline/large shared selection");
                    mutable.change(0xffaabbcc);
                    try (Bitmap recipient = receive(p)) {
                        check(recipient.checksum() == original, "mutable source is always snapshotted");
                        check(recipient.isMutable() == (size == 8), "Android small mutable/large immutable semantics");
                    }
                }
                try (Bitmap shared = mutable.asShared(); Parcel p = new Parcel()) {
                    boolean old = p.pushAllowFds(false);
                    shared.writeToParcel(p, 0);
                    check(!p.hasFileDescriptors(), "FD-forbidden Parcel carries inline bytes");
                    try (Bitmap recipient = receive(p)) {
                        check(!recipient.isShared() && !recipient.isMutable(), "inline immutable result has heap ownership");
                        check(recipient.checksum() == shared.checksum(), "FD-forbidden pixels preserved");
                    }
                    check(!p.pushAllowFds(false), "writer restores exact FD permission");
                    p.restoreAllowFds(old);
                }
            }
        }
        retired(before, "inline and snapshots");
    }

    private static void header(Parcel from, Parcel to, boolean mutable) {
        from.rewind(); from.readInt(); to.writeInt(mutable ? 1 : 0);
        to.writeInt(from.readInt()); to.writeInt(from.readInt());
        int color = from.readInt(); to.writeInt(color);
        if (color != 0) to.writeByteArray(from.createByteArray());
        for (int i = 0; i < 4; ++i) to.writeInt(from.readInt());
        to.writeLong(from.readLong()); to.writeLong(from.readLong());
    }

    private static void mutableMappedInput() {
        long[] before = stats();
        try (Bitmap model = make(96, 96, 0, false, false); Parcel original = new Parcel();
             SharedMemory memory = SharedMemory.create("mutable", 96 * 96 * 4)) {
            model.writeToParcel(original, 0);
            ByteBuffer buffer = memory.mapReadWrite().order(ByteOrder.nativeOrder());
            try (Parcel parcel = new Parcel()) {
                for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x)
                    buffer.putInt((y * 96 + x) * 4, 0xff000000 | ((x * 17 + y * 29) & 0xffffff));
                header(original, parcel, true);
                parcel.writeInt(1); parcel.writeInt(96 * 96 * 4); memory.writeToParcel(parcel, 0);
                try (Bitmap mapped = receive(parcel)) {
                    check(mapped.isMutable() && mapped.isShared() && mapped.checksum() == model.checksum(), "mutable mapped metadata and pixels");
                    mapped.change(0xff765432);
                    check(buffer.getInt(0) == 0xff765432, "mutable fd blob preserves actual shared alias semantics");
                    Bitmap snapshot = mapped.asShared();
                    try (snapshot; Parcel forwarded = new Parcel()) {
                        check(snapshot != mapped && !snapshot.isMutable(), "mutable shared input is copied by asShared");
                        long pixels = mapped.checksum();
                        reset(); mapped.writeToParcel(forwarded, 0);
                        check(stats()[6] == 1 && stats()[5] == 96L * 96 * 4, "mutable backing is never reused by large Parcel forwarding");
                        mapped.change(0xff112233);
                        try (Bitmap copy = receive(forwarded)) {
                            check(!copy.isMutable() && copy.checksum() == pixels, "mutable forward remains independent snapshot");
                        }
                        check(snapshot.checksum() == pixels, "asShared snapshot stays independent");
                    }
                }
            } finally { SharedMemory.unmap(buffer); }
        }
        retired(before, "mutable incoming mapping");
    }

    private static void malformedAndFailures() {
        long[] before = stats();
        try (Bitmap source = make(96, 96, 0, false, false); Parcel original = new Parcel()) {
            source.writeToParcel(original, 0);
            try (SharedMemory shortMemory = SharedMemory.create("short", 4096); Parcel shortParcel = new Parcel()) {
                header(original, shortParcel, false);
                shortParcel.writeInt(1); shortParcel.writeInt(96 * 96 * 4); shortMemory.writeToParcel(shortParcel, 0);
                long[] active = stats();
                failure(() -> receive(shortParcel), BadParcelableException.class, "actual mapping capacity bounds declared size");
                retired(active, "short mapping failure");
            }
            for (int position : new int[] {1, 4, 5, 6, 10, 11}) {
                Object saved = original.values.get(position);
                original.values.set(position, position == 10 ? 9 : -1);
                long[] active = stats();
                failure(() -> receive(original), BadParcelableException.class, "malformed field " + position);
                retired(active, "malformed field " + position);
                original.values.set(position, saved);
            }
            for (int position : new int[] {4, 5, 6, 11}) {
                Object saved = original.values.get(position);
                original.values.set(position, Integer.MAX_VALUE);
                long[] active = stats();
                failure(() -> receive(original), BadParcelableException.class, "overflow/capacity field " + position);
                retired(active, "overflow/capacity field " + position);
                original.values.set(position, saved);
            }
            Object row = original.values.get(6);
            original.values.set(6, 0);
            long[] zeroRow = stats();
            failure(() -> receive(original), BadParcelableException.class, "zero shared row cannot bypass allocation bounds");
            retired(zeroRow, "zero shared row");
            original.values.set(6, row);
            try (Bitmap small = make(8, 7, 0, false, false); Parcel inline = new Parcel()) {
                small.writeToParcel(inline, 0); inline.values.set(6, 0);
                long[] active = stats();
                failure(() -> receive(inline), BadParcelableException.class, "zero inline row cannot create a zero-byte pixel allocation");
                retired(active, "zero inline row");
            }
            for (int point : new int[] {1, 2, 3, 4}) {
                long[] active = stats();
                try (Parcel parcel = new Parcel()) {
                    fail(point);
                    failure(() -> source.writeToParcel(parcel, 0), RuntimeException.class, "writer syscall failure " + point);
                }
                retired(active, "writer syscall failure " + point);
                fail(point);
                failure(() -> source.asShared(), RuntimeException.class, "asShared syscall failure " + point);
                retired(active, "asShared syscall failure " + point);
            }
            for (int point : new int[] {3, 5, 5 + 256, 6}) {
                long[] active = stats(); fail(point);
                failure(() -> receive(original), RuntimeException.class, "reader syscall failure " + point);
                retired(active, "reader syscall failure " + point);
            }
            for (int field = 0; field < 13; ++field) {
                long[] active = stats();
                try (Parcel parcel = new Parcel()) {
                    parcel.failAt = field;
                    failure(() -> source.writeToParcel(parcel, 0), RuntimeException.class, "writer Java fault " + field);
                }
                retired(active, "writer Java fault");
                original.rewind(); original.failAt = field;
                failure(() -> Bitmap.CREATOR.createFromParcel(original), RuntimeException.class, "reader Java fault " + field);
                retired(active, "reader Java fault");
            }
            original.rewind();
            long[] active = stats();
            SharedMemory.failClose = true;
            failure(() -> receive(original), RuntimeException.class, "close after native mapping adoption");
            retired(active, "close failure after adoption");
            try (Parcel parcel = new Parcel()) {
                parcel.failFdAfterDup = true;
                failure(() -> source.writeToParcel(parcel, 0), RuntimeException.class, "Parcel holds descriptor then throws");
            }
            retired(active, "partially written Parcel cleanup");
        }
        retired(before, "all failure paths");
    }

    private static void parallelRecipients() throws Exception {
        long[] before = stats();
        try (Bitmap mutable = make(192, 108, 0, false, false); Bitmap source = mutable.asShared()) {
            final long pixels = source.checksum();
            CountDownLatch start = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            List<Thread> threads = new ArrayList<>();
            for (int i = 0; i < 6; ++i) {
                Thread thread = new Thread(() -> {
                    try {
                        start.await();
                        for (int n = 0; n < 40; ++n) try (Parcel parcel = new Parcel()) {
                            source.writeToParcel(parcel, 0);
                            try (Bitmap recipient = receive(parcel)) {
                                check(recipient.asShared() == recipient && recipient.checksum() == pixels, "independent concurrent recipient");
                            }
                        }
                    } catch (Throwable error) { failure.compareAndSet(null, error); }
                });
                threads.add(thread); thread.start();
            }
            start.countDown(); for (Thread thread : threads) thread.join();
            if (failure.get() != null) throw new AssertionError("parallel recipients", failure.get());
        }
        retired(before, "parallel recipients");
    }

    private static void measure(int width, int height, boolean shared) {
        long[] before = stats();
        Bitmap source = make(width, height, 0, false, false);
        if (shared) { Bitmap next = source.asShared(); source.recycle(); source = next; }
        long expected = source.checksum(); reset();
        final int hops = 20;
        try {
            for (int hop = 0; hop < hops; ++hop) {
                Bitmap next;
                try (Parcel parcel = new Parcel()) { source.writeToParcel(parcel, 0); next = receive(parcel); }
                source.recycle(); source = next;
                check(source.checksum() == expected, "work measurement preserves every pixel at each hop");
            }
            long[] work = stats();
            System.out.printf("WORK {\"width\":%d,\"height\":%d,\"initial_shared\":%s,\"hops\":%d,\"heap_calls\":%d,\"heap_bytes\":%d,\"copy_calls\":%d,\"copy_bytes\":%d,\"memfd_calls\":%d,\"memfd_bytes\":%d,\"maps\":%d,\"dup_calls\":%d,\"checksum\":\"%d\"}%n",
                width, height, shared, hops, work[2], work[3], work[4], work[5], work[6], work[7], work[8], work[9], expected);
        } finally { source.recycle(); }
        retired(before, "measured chain");
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // Initialize both Java transport paths before taking fd lifetime baselines.
        try (Bitmap warm = make(96, 96, 0, false, false); Parcel parcel = new Parcel()) {
            warm.writeToParcel(parcel, 0); receive(parcel).recycle();
        }
        if (!args[1].equals("work")) {
            ownership(8, 7, 8); ownership(96, 77, 12); ownership(1920, 1080, 0);
            inlineAndSnapshots(); mutableMappedInput(); malformedAndFailures(); parallelRecipients();
            System.out.println("Bitmap shared production JNI: " + checks + " checks passed");
        }
        for (int[] size : new int[][] {{1280,720}, {1920,1080}, {3840,2160}}) {
            measure(size[0], size[1], false); measure(size[0], size[1], true);
        }
        check(stats()[1] == 0, "no surviving native mappings");
    }
}
