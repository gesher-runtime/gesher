package gesher.services;

import android.media.MediaCodec;
import android.media.MediaFormat;
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

    public static void main(String[] args) throws Exception {
        // Standalone ART callers need the framework natives used by MediaCodec's Trace calls.
        System.loadLibrary("android_runtime");
        ordinaryLifecycle();
        retireBlockedCall(false, true);
        retireBlockedCall(true, true);
        retireBlockedCall(false, false);
        System.out.println("CodecLifecycleContractTest: " + checks + " checks passed; native=true");
    }
}
