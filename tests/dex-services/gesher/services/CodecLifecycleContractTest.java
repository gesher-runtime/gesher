package gesher.services;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Public SDK calls through the real ART/JNI receiver; no substituted MediaCodec. */
public final class CodecLifecycleContractTest {
    private static int checks;

    private static void check(boolean ok, String reason) {
        checks++;
        if (!ok) throw new AssertionError(reason);
    }

    private static MediaFormat format() {
        return MediaFormat.createAudioFormat("audio/raw", 8000, 1);
    }

    private static MediaCodec started() throws Exception {
        MediaCodec codec = MediaCodec.createDecoderByType("audio/raw");
        try {
            codec.configure(format(), null, null, 0);
            codec.start();
            return codec;
        } catch (Throwable failure) {
            codec.release();
            throw failure;
        }
    }

    private static void await(CountDownLatch latch, String reason) throws Exception {
        if (!latch.await(5, TimeUnit.SECONDS)) {
            // A native wait regression must end this bounded process, rather
            // than hang the runner or dispose objects still in use by it.
            System.err.println("FAIL CodecLifecycleContractTest: " + reason);
            Runtime.getRuntime().halt(2);
        }
        check(true, reason);
    }

    private static Thread daemon(String name, Runnable body) {
        Thread thread = new Thread(body, name);
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static void ordinaryLifecycle() throws Exception {
        MediaCodec codec = started();
        try {
            int first = codec.dequeueInputBuffer(0);
            check(first >= 0, "started codec supplies an input slot");
            check(codec.getInputBuffer(first) != null, "ordinary input buffer remains available");
            codec.flush();
            check(codec.dequeueInputBuffer(0) >= 0, "completed flush restores input slots");
            codec.stop();
            codec.configure(format(), null, null, 0);
            codec.start();
            check(codec.dequeueInputBuffer(0) >= 0, "codec can configure and start after stop");
            codec.reset();
            codec.configure(format(), null, null, 0);
            codec.start();
            check(codec.dequeueInputBuffer(0) >= 0, "codec can configure and start after reset");
        } finally {
            codec.release();
        }
    }

    private static void retireBlockedCall(boolean input, boolean release) throws Exception {
        MediaCodec codec = started();
        try {
            if (input) {
                int occupied = 0;
                while (codec.dequeueInputBuffer(0) >= 0) {
                    occupied++;
                    check(occupied <= 256, "input slot inventory is bounded");
                }
                check(occupied > 0, "all available input slots are held by the client");
            }
            CountDownLatch entering = new CountDownLatch(1);
            CountDownLatch returned = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            daemon("codec-blocked-dequeue", () -> {
                entering.countDown();
                try {
                    int result = input ? codec.dequeueInputBuffer(-1)
                            : codec.dequeueOutputBuffer(new MediaCodec.BufferInfo(), -1);
                    if (result != MediaCodec.INFO_TRY_AGAIN_LATER) {
                        failure.set(new AssertionError("retired dequeue delivered index " + result));
                    }
                } catch (IllegalStateException expectedDuringRetirement) {
                    // Android may report retirement as an IllegalStateException.
                } catch (Throwable unexpected) {
                    failure.set(unexpected);
                } finally {
                    returned.countDown();
                }
            });
            await(entering, "dequeue thread reaches its native call boundary");
            check(!returned.await(100, TimeUnit.MILLISECONDS),
                    "unavailable buffer keeps the negative-timeout call pending");
            CountDownLatch retired = new CountDownLatch(1);
            daemon("codec-retirement", () -> {
                try {
                    if (release) codec.release();
                    else codec.stop();
                } catch (Throwable unexpected) {
                    failure.compareAndSet(null, unexpected);
                } finally {
                    retired.countDown();
                }
            });
            await(retired, "receiver retirement is bounded");
            await(returned, "receiver retirement wakes the pending dequeue");
            check(failure.get() == null, "retirement preserves a valid JNI result: " + failure.get());
            if (release) {
                codec.release();
                boolean rejected = false;
                try { codec.start(); } catch (IllegalStateException expected) { rejected = true; }
                check(rejected, "released receiver cannot restart");
            } else {
                codec.configure(format(), null, null, 0);
                codec.start();
                check(codec.dequeueInputBuffer(0) >= 0, "new configuration survives old call retirement");
            }
        } finally {
            codec.release();
        }
    }

    private static void asynchronousLifecycle() throws Exception {
        HandlerThread callbacks = new HandlerThread("codec-java-callbacks");
        callbacks.start();
        Handler handler = new Handler(callbacks.getLooper());
        MediaCodec codec = MediaCodec.createDecoderByType("audio/raw");
        CountDownLatch finished = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        // These fields are confined to the callback Handler.
        int[] phase = {0}, formats = {0}, outputs = {0};
        boolean[] sent = {false};
        Runnable failed = () -> {
            try { codec.release(); }
            catch (Throwable cleanup) { failure.compareAndSet(null, cleanup); }
            finished.countDown();
        };
        try {
            codec.setCallback(new MediaCodec.Callback() {
                @Override public void onInputBufferAvailable(MediaCodec owner, int index) {
                    try {
                        if (phase[0] == 0) {
                            phase[0] = 1;
                            owner.flush();
                            // AOSP permits callbacks already queued before flush.
                            // Drain that Handler prefix before starting the new epoch.
                            handler.post(() -> {
                                try {
                                    phase[0] = 2;
                                    // This public setter enters native code while
                                    // holding the same Java listener monitor as posts.
                                    codec.setOnFrameRenderedListener((c, mediaUs, systemNs) -> {}, handler);
                                    codec.start();
                                } catch (Throwable unexpected) {
                                    failure.compareAndSet(null, unexpected);
                                    failed.run();
                                }
                            });
                            return;
                        }
                        if (phase[0] != 2 || sent[0]) return;
                        sent[0] = true;
                        ByteBuffer input = owner.getInputBuffer(index);
                        if (input == null || input.capacity() < 2) {
                            throw new AssertionError("callback has no owned PCM input");
                        }
                        input.put(0, (byte) 0);
                        input.put(1, (byte) 0);
                        owner.queueInputBuffer(index, 0, 2, 1234, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    } catch (Throwable unexpected) {
                        failure.compareAndSet(null, unexpected);
                        failed.run();
                    }
                }
                @Override public void onOutputFormatChanged(MediaCodec owner, MediaFormat output) {
                    try {
                        formats[0]++;
                        if (output.getInteger(MediaFormat.KEY_SAMPLE_RATE) != 8000
                                || output.getInteger(MediaFormat.KEY_CHANNEL_COUNT) != 1) {
                            throw new AssertionError("callback PCM format differs from real decoder");
                        }
                    } catch (Throwable unexpected) {
                        failure.compareAndSet(null, unexpected);
                        failed.run();
                    }
                }
                @Override public void onOutputBufferAvailable(MediaCodec owner, int index,
                        MediaCodec.BufferInfo info) {
                    try {
                        outputs[0]++;
                        if (info.size > 0) {
                            ByteBuffer output = owner.getOutputBuffer(index);
                            if (output == null || info.size != 2 || info.presentationTimeUs != 1234
                                    || output.get(info.offset) != 0 || output.get(info.offset + 1) != 0) {
                                throw new AssertionError("callback lost real PCM data or timestamp");
                            }
                        }
                        owner.releaseOutputBuffer(index, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            phase[0] = 3;
                            owner.stop();
                            owner.release();
                            finished.countDown();
                        }
                    } catch (Throwable unexpected) {
                        failure.compareAndSet(null, unexpected);
                        failed.run();
                    }
                }
                @Override public void onError(MediaCodec owner, MediaCodec.CodecException error) {
                    failure.compareAndSet(null, error);
                    failed.run();
                }
            }, handler);
            codec.configure(format(), null, null, 0);
            codec.start();
            await(finished, "public Java callbacks flush/restart/stop/release on real ART/JNI");
            check(failure.get() == null, "Java callback lifecycle: " + failure.get());
            check(phase[0] == 3 && sent[0] && formats[0] == 1 && outputs[0] == 2,
                    "restarted Java callback epoch emits actual PCM then EOS once");
        } finally {
            codec.release();
            callbacks.quitSafely();
            callbacks.join(2000);
            check(!callbacks.isAlive(), "Java callback Handler retires");
        }
    }

    public static void main(String[] args) throws Exception {
        // Standalone ART callers need the framework natives used by MediaCodec's Trace calls.
        System.loadLibrary("android_runtime");
        ordinaryLifecycle();
        retireBlockedCall(false, true);
        retireBlockedCall(true, true);
        retireBlockedCall(false, false);
        asynchronousLifecycle();
        System.out.println("CodecLifecycleContractTest: " + checks + " checks passed; native=true");
    }
}
