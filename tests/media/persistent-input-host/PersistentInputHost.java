import android.media.MediaCodec;
import android.view.Surface;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicLong;

public final class PersistentInputHost {
    private static native long attach(Surface surface, int width, int height);
    private static native void closeLease(long lease);
    private static native int frames(long lease);
    private static native boolean post(Surface surface);
    private static native int count(int which);
    private static native void gate(int operation);
    private static native void failWrapper(boolean value);
    private static int checks;
    private static void require(boolean value, String reason) {
        ++checks;
        if (!value) throw new AssertionError(reason);
    }
    private static void argument(Runnable operation) {
        try { operation.run(); throw new AssertionError("missing IllegalArgumentException"); }
        catch (IllegalArgumentException expected) { ++checks; }
    }
    private static void state(Runnable operation) {
        try { operation.run(); throw new AssertionError("missing IllegalStateException"); }
        catch (IllegalStateException expected) { ++checks; }
    }
    private static void join(Thread thread) throws Exception {
        thread.join(5_000);
        require(!thread.isAlive(), "bounded native worker completion");
    }

    private static void reuseAndErrors() throws Exception {
        Surface surface = MediaCodec.createPersistentInputSurface();
        require(surface != null && surface.isValid(), "factory creates a native-backed persistent Surface");
        int queues = count(1);
        require(count(0) == 1 && count(2) == 1, "one producer and one native Java reference");
        argument(() -> attach(null, 2, 2));
        argument(() -> attach(new Surface(), 2, 2));
        Field token = surface.getClass().getDeclaredField("mPersistentObject");
        token.setAccessible(true);
        long saved = token.getLong(surface);
        token.setLong(surface, Long.MAX_VALUE);
        argument(() -> attach(surface, 2, 2));
        token.setLong(surface, saved);
        require(post(surface), "unattached persistent source accepts a producer frame");
        for (int i = 0; i < 200; ++i) {
            long lease = attach(surface, 2 + i % 3, 2 + i % 2);
            require(lease != 0, "successive consumer has a lease");
            state(() -> attach(surface, 2, 2));
            require(post(surface) && frames(lease) == 1, "consumer gets an actual posted frame");
            closeLease(lease);
            closeLease(lease);
            require(surface.isValid() && post(surface), "retirement preserves the producer");
        }
        require(count(1) == queues, "two hundred sessions allocate one producer");
        long last = attach(surface, 2, 2);
        surface.release(); surface.release();
        require(!surface.isValid() && count(2) == 0, "explicit double release closes native Java reference once");
        argument(() -> attach(surface, 2, 2));
        require(count(0) == 1, "native consumer retains an explicitly released source");
        closeLease(last);
        require(count(0) == 0, "last consumer releases native producer");
    }

    private static void concurrentRelease() throws Exception {
        Surface surface = MediaCodec.createPersistentInputSurface();
        long lease = attach(surface, 2, 2);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        gate(0);
        Thread producer = new Thread(() -> {
            try { require(post(surface), "admitted frame completes after Java Surface release"); }
            catch (Throwable error) { failure.set(error); }
        });
        producer.start(); gate(1);
        Thread javaRelease = new Thread(surface::release);
        javaRelease.start(); join(javaRelease);
        require(count(0) == 1, "Java release leaves the admitted lease and producer call alive");
        CountDownLatch retired = new CountDownLatch(1), retiring = new CountDownLatch(1);
        Thread consumer = new Thread(() -> {
            retiring.countDown(); closeLease(lease); retired.countDown();
        });
        consumer.start();
        require(retiring.await(5, TimeUnit.SECONDS), "native retirement started");
        require(!retired.await(100, TimeUnit.MILLISECONDS), "consumer retirement waits for callback completion");
        Surface independent = MediaCodec.createPersistentInputSurface();
        long other = attach(independent, 2, 2);
        closeLease(other); independent.release();
        require(count(0) == 1, "quiescing one producer does not hold the shared JNI registry lock");
        gate(2); join(producer); join(consumer);
        if (failure.get() != null) throw new AssertionError(failure.get());
        require(count(0) == 0 && count(2) == 0, "concurrent release balances producer and wrapper ownership");
    }

    private static void factoryFailures() {
        for (int mode = 1; mode <= 2; ++mode) {
            Surface.failTransfer = mode;
            state(MediaCodec::createPersistentInputSurface);
            Surface.failTransfer = 0;
            require(count(0) == 0 && count(2) == 0, "factory failure releases before/after transferred native ownership");
        }
        failWrapper(true);
        try { MediaCodec.createPersistentInputSurface(); throw new AssertionError("missing wrapper allocation failure"); }
        catch (OutOfMemoryError expected) { ++checks; }
        finally { failWrapper(false); }
        require(count(0) == 0 && count(2) == 0, "wrapper creation failure leaves no native source or Java owner");
    }

    private static void lookupReleaseRace() throws Exception {
        Field lockField = Surface.class.getDeclaredField("mLock");
        lockField.setAccessible(true);
        for (int iteration = 0; iteration < 100; ++iteration) {
            Surface surface = MediaCodec.createPersistentInputSurface();
            Object lock = lockField.get(surface);
            AtomicLong acquired = new AtomicLong();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            CountDownLatch ready = new CountDownLatch(2);
            Thread attach = new Thread(() -> {
                ready.countDown();
                try { acquired.set(attach(surface, 2, 2)); }
                catch (IllegalArgumentException alreadyReleased) {}
                catch (Throwable error) { failure.set(error); }
            });
            Thread release = new Thread(() -> {
                ready.countDown();
                try { surface.release(); }
                catch (Throwable error) { failure.set(error); }
            });
            synchronized (lock) {
                attach.start(); release.start();
                require(ready.await(5, TimeUnit.SECONDS), "lookup and release both contend for Surface.mLock");
            }
            join(attach); join(release);
            if (failure.get() != null) throw new AssertionError(failure.get());
            if (acquired.get() != 0) closeLease(acquired.get());
            require(!surface.isValid() && count(0) == 0 && count(2) == 0,
                    "racing lookup/release either retains an admitted owner or reports released, without leaks");
        }
    }

    private static WeakReference<Surface> unretained() {
        return new WeakReference<>(MediaCodec.createPersistentInputSurface());
    }
    private static void finalization() throws Exception {
        WeakReference<Surface> surface = unretained();
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while ((surface.get() != null || count(0) != 0) && System.nanoTime() < until) {
            System.gc(); System.runFinalization(); Thread.sleep(10);
        }
        require(surface.get() == null && count(0) == 0 && count(2) == 0,
                "Surface finalization invokes persistent release and retires both native owners");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        reuseAndErrors(); concurrentRelease(); lookupReleaseRace(); factoryFailures(); finalization();
        System.out.println("persistent input JNI contracts: " + checks + " checks; no live native producers or wrappers");
    }
}
