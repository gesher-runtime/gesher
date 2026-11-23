// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Actual production MediaRecorder JNI + Recorder state on a checked host JVM.
// Audio capture/encoding/Surface are declared fixture boundaries, not devices.
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.concurrent.atomic.AtomicInteger;

public final class MediaRecorderStateHost implements AutoCloseable {
    long context;
    static int checks;
    static final AtomicInteger errors = new AtomicInteger();
    static native void controls(boolean denied, boolean captureFailure);
    static native void feed(int batches);
    static native boolean captureReleased();
    static native int encodedParameter(int which);
    native void setup(Object weak, String name, Object attribution);
    native void release();
    native void reset();
    native void source(int value);
    native void videoSource(int value);
    native void format(int value);
    native void audioEncoder(int value);
    native void videoEncoder(int value);
    native void videoRate(int value);
    native void duration(int value);
    native void size(long value);
    native void parameter(String value);
    native void file(FileDescriptor fd) throws IOException;
    native void nextFile(FileDescriptor fd) throws IOException;
    native void prepare() throws IOException;
    native void start();
    native void pause();
    native void resume();
    native void stop();
    native int amplitude();
    native void camera(Object camera);
    native int[] routes();
    MediaRecorderStateHost() { setup(this, "host", null); }
    public void close() { release(); controls(false, false); }
    static void event(Object weak, int what, int extra, int unused, Object data) {
        if (what == 1) errors.incrementAndGet();
    }
    interface Operation { void run() throws Exception; }
    static void expect(Class<? extends Throwable> exact, Operation operation) throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            if (failure.getClass() != exact) throw new AssertionError("expected " + exact + ", got " + failure, failure);
            checks++;
            return;
        }
        throw new AssertionError("missing " + exact.getName());
    }
    static void check(boolean value, String description) {
        if (!value) throw new AssertionError(description);
        checks++;
    }
    void config(File target, int codec) throws Exception {
        source(1); format(2); audioEncoder(codec);
        parameter("audio-param-sampling-rate=48000; audio-param-encoding-bitrate=96000");
        try (RandomAccessFile output = new RandomAccessFile(target, "rw")) { file(output.getFD()); }
    }
    void expectError() throws Exception {
        expect(IllegalStateException.class, () -> source(1));
        expect(IllegalStateException.class, this::prepare);
        expect(IllegalStateException.class, this::start);
        expect(IllegalStateException.class, this::amplitude);
        expect(IllegalStateException.class, () -> duration(500));
    }
    static void parameters(File target) throws Exception {
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            check(r.routes().length == 0, "an idle recorder cannot invent an input route");
            r.parameter(null);
            for (int value : new int[]{1, 100}) expect(RuntimeException.class, () -> r.duration(value));
            for (long value : new long[]{1, 1024}) expect(RuntimeException.class, () -> r.size(value));
            r.duration(-1); r.duration(0); r.duration(101);
            r.size(-1); r.size(0); r.size(1025); r.size(0);
            for (String value : new String[]{"", "=1", "max-duration=abc", "max-filesize=9223372036854775808",
                    "audio-param-number-of-channels=3", "video-param-rotation-angle-degrees=-90",
                    "video-param-rotation-angle-degrees=45", "unknown=5", "max-duration=200;"}) {
                expect(RuntimeException.class, () -> r.parameter(value));
            }
            r.parameter("max-duration=0; max-filesize=0; video-param-rotation-angle-degrees=450");
            r.config(target, 3); r.prepare();
            expect(IllegalStateException.class, () -> r.duration(1000));
            expect(IllegalStateException.class, () -> r.parameter("max-filesize=2000"));
        }
    }
    static void ordering(File target) throws Exception {
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            expect(IllegalStateException.class, () -> r.format(2));
            expect(IllegalStateException.class, () -> r.audioEncoder(3));
            expect(IllegalStateException.class, r::prepare);
            expect(IllegalStateException.class, r::start);
            expect(IllegalStateException.class, r::pause);
            expect(IllegalStateException.class, r::resume);
            expect(IllegalStateException.class, r::stop);
            expect(IllegalArgumentException.class, () -> r.source(77));
            r.source(1);
            expect(IllegalStateException.class, () -> r.source(1));
            r.format(2);
            expect(IllegalStateException.class, () -> r.format(2));
            expect(IllegalStateException.class, () -> r.videoSource(2));
            expect(IllegalStateException.class, () -> r.videoEncoder(2));
            try (RandomAccessFile output = new RandomAccessFile(target, "rw")) {
                r.file(output.getFD());
                expect(IllegalStateException.class, () -> r.file(output.getFD()));
            }
            expect(IllegalStateException.class, r::prepare); // source/encoder mismatch is nonfatal
            expect(IllegalArgumentException.class, () -> r.audioEncoder(77));
            r.audioEncoder(3);
            expect(IllegalStateException.class, () -> r.audioEncoder(3));
            r.prepare();
            check(r.routes().length == 0, "prepared capture is not an active route");
            expect(IllegalStateException.class, r::prepare);
            expect(IllegalStateException.class, r::pause);
            r.start();
            check(r.routes().length == 1 && r.routes()[0] == 3, "active default capture route");
            expect(IllegalStateException.class, r::start);
            r.resume(); r.pause(); r.pause();
            check(r.routes().length == 0, "paused capture has no active input route");
            r.resume(); r.resume();
            expect(RuntimeException.class, r::stop); // no samples is a backend stop failure
            r.expectError();
            r.reset(); r.config(target, 3); r.prepare();
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            r.source(1); r.format(2); r.audioEncoder(3);
            expect(IllegalStateException.class, r::prepare); // backend INVALID_OPERATION: no fd
            r.expectError();
            r.reset(); r.config(target, 3); r.prepare();
        }
    }
    static void failures(File target) throws Exception {
        for (int codec : new int[]{0, 1, 2, 4, 5, 6}) {
            try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
                r.config(target, codec);
                expect(IOException.class, r::prepare);
                r.expectError();
                r.reset(); r.config(target, 3); r.prepare();
            }
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            r.config(target, 3); r.prepare(); controls(false, true);
            expect(RuntimeException.class, r::start);
            check(captureReleased(), "failed start releases the real recorder's capture seam");
            r.expectError(); controls(false, false); r.reset(); r.config(target, 3); r.prepare();
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            r.config(target, 3); r.prepare(); r.start(); feed(3); r.pause();
            int previous = errors.get(); controls(false, true);
            expect(RuntimeException.class, r::resume);
            check(captureReleased(), "failed resume releases before returning");
            check(errors.get() == previous + 1, "failed resume has one terminal error");
            expect(IllegalStateException.class, r::stop);
            r.expectError(); controls(false, false); r.reset(); r.config(target, 3); r.prepare();
        }
    }
    static void descriptors(File target) throws Exception {
        try (RandomAccessFile file = new RandomAccessFile(target, "rw")) { file.setLength(400000); }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            r.source(1); r.format(2); r.audioEncoder(3);
            try (RandomAccessFile readOnly = new RandomAccessFile(target, "r")) {
                expect(IOException.class, () -> r.file(readOnly.getFD()));
            }
            try (RandomAccessFile output = new RandomAccessFile(target, "rw")) { r.file(output.getFD()); }
            check(target.length() == 0, "setOutputFile truncates stale trailing bytes");
            r.prepare();
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            r.source(1); r.format(2); r.audioEncoder(3);
            expect(IOException.class, () -> r.file(new FileDescriptor()));
            r.expectError();
        }
    }
    static void nextDescriptors(File target) throws Exception {
        File next = new File(target + ".next");
        File rejected = new File(target + ".rejected");
        try (MediaRecorderStateHost r = new MediaRecorderStateHost();
                RandomAccessFile out = new RandomAccessFile(next, "rw")) {
            expect(IllegalArgumentException.class, () -> r.nextFile(null));
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            expect(IllegalStateException.class, () -> r.nextFile(new FileDescriptor()));
            try (RandomAccessFile readOnly = new RandomAccessFile(next, "r")) {
                expect(IOException.class, () -> r.nextFile(readOnly.getFD()));
            }
            r.config(target, 3);
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            expect(IllegalStateException.class, () -> r.nextFile(new FileDescriptor()));
            r.prepare();
            expect(IOException.class, () -> r.nextFile(new FileDescriptor()));
            try (RandomAccessFile readOnly = new RandomAccessFile(next, "r")) {
                expect(IOException.class, () -> r.nextFile(readOnly.getFD()));
            }
            out.setLength(10000);
            r.nextFile(out.getFD());
            check(next.length() == 0, "pending file is truncated on admission");
            try (RandomAccessFile second = new RandomAccessFile(rejected, "rw")) {
                second.setLength(7000);
                expect(IllegalStateException.class, () -> r.nextFile(second.getFD()));
                check(rejected.length() == 0, "Stagefright truncates before duplicate-pending rejection");
            }
            r.start(); r.pause();
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            r.resume(); feed(3); r.stop();
            check(target.length() > 0 && next.length() == 0, "unused pending file stays unused on stop");
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            expect(IllegalStateException.class, () -> r.nextFile(new FileDescriptor()));
            // Failed argument/admission calls above never put the recorder into ERROR.
            r.config(target, 3); r.prepare(); r.nextFile(out.getFD()); r.reset();
            r.config(target, 3); r.prepare(); r.nextFile(out.getFD());
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost();
                RandomAccessFile out = new RandomAccessFile(next, "rw")) {
            r.source(1); r.format(1); r.audioEncoder(3); // THREE_GPP is not MPEG_4.
            try (RandomAccessFile first = new RandomAccessFile(target, "rw")) { r.file(first.getFD()); }
            r.prepare();
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            expect(IllegalStateException.class, () -> r.nextFile(new FileDescriptor()));
            r.release();
            expect(IllegalArgumentException.class, () -> r.nextFile(null));
            expect(IllegalStateException.class, () -> r.nextFile(out.getFD()));
            expect(IllegalStateException.class, () -> r.nextFile(new FileDescriptor()));
        }
    }
    static void clipping(File target) throws Exception {
        for (int[] row : new int[][]{{3, 96000, 999999999, 48000, 192000},
                {3, 1000, 1, 44100, 96000}, {7, 96000, 999999999, 48000, 510000},
                {7, 1000, 1, 8000, 6000}}) {
            try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
                r.config(target, row[0]);
                r.parameter("audio-param-sampling-rate=" + row[1] + ";audio-param-encoding-bitrate=" + row[2]);
                r.prepare();
                check(encodedParameter(0) == row[3], "clip to actual sample-rate bounds");
                check(encodedParameter(1) == row[4], "clip to actual bitrate bounds");
            }
        }
        for (int[] row : new int[][]{{3, 45000}, {7, 44100}}) {
            try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
                r.config(target, row[0]); r.parameter("audio-param-sampling-rate=" + row[1]);
                expect(IOException.class, r::prepare); r.expectError();
            }
        }
    }
    static void sourcesAndVideo(File target) throws Exception {
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            controls(true, false);
            expect(RuntimeException.class, () -> r.source(1));
            r.expectError(); controls(false, false); r.reset(); r.config(target, 3); r.prepare();
        }
        for (int source : new int[]{2, 3, 4, 8, 1998}) {
            try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
                expect(RuntimeException.class, () -> r.source(source)); r.expectError();
            }
        }
        try (MediaRecorderStateHost r = new MediaRecorderStateHost()) {
            expect(NullPointerException.class, () -> r.camera(null));
            expect(RuntimeException.class, () -> r.camera(new Object()));
            r.expectError(); r.reset(); r.videoSource(2); r.format(2);
            expect(IllegalArgumentException.class, () -> r.videoRate(0));
            expect(RuntimeException.class, () -> r.videoRate(1001));
            r.expectError();
        }
        MediaRecorderStateHost released = new MediaRecorderStateHost(); released.close();
        expect(IllegalStateException.class, () -> released.source(1));
        expect(IllegalArgumentException.class, () -> released.source(-1));
        expect(IllegalStateException.class, released::reset);
        expect(IllegalStateException.class, released::routes);
        released.parameter(null); released.release();
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); File target = new File(args[1], "jni-state.m4a");
        parameters(target); ordering(target); failures(target); descriptors(target); nextDescriptors(target);
        clipping(target); sourcesAndVideo(target);
        System.out.println("MediaRecorder production JNI + state: " + checks + " checks passed");
    }
}
