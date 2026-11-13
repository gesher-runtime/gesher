/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.os.Binder;
import android.os.Looper;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Production helper; declared constructor/native seams supplied by the host driver. */
public final class SettingsContextBootstrapHost {
    private static int checks;
    private static synchronized void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    public static void main(String[] args) throws Exception {
        try { GesherSettingsContext.get(); throw new AssertionError("Unprepared context was accepted"); }
        catch (IllegalStateException expected) { check(true, "Unprepared context rejected"); }
        try { GesherSettingsContext.prepare(); throw new AssertionError("Missing main Looper was accepted"); }
        catch (IllegalStateException expected) { check(true, "Missing Looper rejected"); }
        Looper.prepareMainLooper();
        Object current = ActivityThread.current;
        try { GesherSettingsContext.prepare(); }
        catch (IllegalStateException earlyLookup) {
            throw new AssertionError("Bootstrap queried the display service before publication", earlyLookup);
        }
        GesherSettingsContext.prepare();
        check(ActivityThread.constructions == 1, "Main-thread identity must be captured once");
        check(ContextImpl.constructions == 0, "Bootstrap constructed a service-dependent context");
        check(ActivityThread.current == current, "Bootstrap replaced the current ActivityThread");

        Binder.restoreCallingIdentity(10042L);
        try { GesherSettingsContext.get(); throw new AssertionError("Unavailable service was hidden"); }
        catch (IllegalStateException expected) { check(true, "Failed construction remains retryable"); }
        check(Binder.getCallingIdentity() == 10042L, "Failure did not restore caller identity");
        ContextImpl.servicesReady = true;
        Context[] contexts = new Context[8];
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch start = new CountDownLatch(1);
        Thread[] workers = new Thread[contexts.length];
        for (int i = 0; i < workers.length; i++) {
            final int index = i;
            workers[i] = new Thread(() -> {
                try {
                    start.await();
                    Binder.restoreCallingIdentity(20000L + index);
                    check(Looper.myLooper() == null, "Worker unexpectedly owns a Looper");
                    contexts[index] = GesherSettingsContext.get();
                    check(Binder.getCallingIdentity() == 20000L + index, "Success did not restore caller identity");
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            });
            workers[i].start();
        }
        start.countDown();
        for (Thread worker : workers) worker.join();
        if (failure.get() != null) throw new AssertionError("Worker context construction failed", failure.get());
        check(ContextImpl.constructions == 1, "Concurrent callers created duplicate contexts");
        for (Context context : contexts) check(context == contexts[0], "Context identity differs between callers");
        check(((ActivityThread) contexts[0].thread).looper == Looper.getMainLooper(), "Context lost its main Looper identity");
        check(contexts[0].identity == 0L, "Internal context construction inherited an external Binder caller");
        check(ActivityThread.current == current, "Context construction replaced the application's thread");
        check(GesherSettingsContext.get() == contexts[0], "Published context was not retained");
        System.out.println("Settings context bootstrap contracts passed: " + checks + " checks");
    }
}
