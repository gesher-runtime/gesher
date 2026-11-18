/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.content.pm.FeatureInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.SystemFeaturesCache;
import android.os.Binder;
import android.os.Looper;
import android.os.RemoteException;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Production helper; declared constructor/native seams supplied by the host driver. */
public final class SettingsContextBootstrapHost {
    private static int checks;
    private static synchronized void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static FeatureInfo feature(String name, int version) {
        FeatureInfo result = new FeatureInfo();
        result.name = name;
        result.version = version;
        return result;
    }
    private static final class PackageManagerFixture implements IPackageManager {
        final List<FeatureInfo> features = List.of(
                feature("fixture.first", 7), feature("fixture.second", 12));
        int queries;
        long identity = -1L;
        RemoteException failure;
        @Override public ParceledListSlice<FeatureInfo> getSystemAvailableFeatures()
                throws RemoteException {
            queries++;
            identity = Binder.getCallingIdentity();
            if (failure != null) throw failure;
            return new ParceledListSlice<>(features);
        }
    }
    public static void main(String[] args) throws Exception {
        check(args.length == 1 && (args[0].equals("initialize") || args[0].equals("retained")),
                "A fresh-JVM cache mode is required");
        boolean retained = args[0].equals("retained");
        try { GesherSettingsContext.get(); throw new AssertionError("Unprepared context was accepted"); }
        catch (IllegalStateException expected) { check(true, "Unprepared context rejected"); }
        try { GesherSettingsContext.prepare(); throw new AssertionError("Missing main Looper was accepted"); }
        catch (IllegalStateException expected) { check(true, "Missing Looper rejected"); }
        Looper.prepareMainLooper();
        PackageManagerFixture packages = new PackageManagerFixture();
        SystemFeaturesCache initialCache = retained
                ? new SystemFeaturesCache(List.of(feature("fixture.retained", 42))) : null;
        if (retained) SystemFeaturesCache.setInstance(initialCache);
        Object current = ActivityThread.current;
        try { GesherSettingsContext.prepare(); }
        catch (IllegalStateException earlyLookup) {
            throw new AssertionError("Bootstrap queried the display service before publication", earlyLookup);
        }
        GesherSettingsContext.prepare();
        check(ActivityThread.constructions == 1, "Main-thread identity must be captured once");
        check(ContextImpl.constructions == 0, "Bootstrap constructed a service-dependent context");
        check(ActivityThread.current == current, "Bootstrap replaced the current ActivityThread");
        check(ContextImpl.attempts == 0, "Bootstrap attempted service-dependent construction");
        check(ActivityThread.packageManagerLookups == 0 && packages.queries == 0,
                "Bootstrap queried package-manager features");
        check(SystemFeaturesCache.fixtureInstance() == initialCache,
                "Bootstrap changed the existing feature cache");
        check(SystemFeaturesCache.constructions == (retained ? 1 : 0)
                        && SystemFeaturesCache.publications == (retained ? 1 : 0),
                "Bootstrap constructed or published a feature cache");

        Binder.restoreCallingIdentity(10042L);
        if (!retained) {
            try { GesherSettingsContext.get(); throw new AssertionError("Missing package manager was hidden"); }
            catch (IllegalStateException expected) {
                check("Package manager is not available".equals(expected.getMessage()),
                        "Missing package manager lost its lifecycle error");
            }
            check(Binder.getCallingIdentity() == 10042L, "Missing package manager lost caller identity");
            check(ActivityThread.packageManagerLookupIdentity == 0L,
                    "Package-manager lookup inherited the external caller");
            check(!SystemFeaturesCache.hasInstance(), "Missing package manager published a cache");
            check(ContextImpl.attempts == 0, "Missing package manager reached context construction");

            ActivityThread.packageManager = packages;
            RemoteException remoteFailure = new RemoteException("Fixture IPC failure");
            packages.failure = remoteFailure;
            try { GesherSettingsContext.get(); throw new AssertionError("Package-manager IPC failure was hidden"); }
            catch (RuntimeException expected) {
                check(expected == remoteFailure.translated && expected.getCause() == remoteFailure,
                        "IPC failure lost its system-server translation or cause");
            }
            check(remoteFailure.translations == 1, "IPC failure did not use system-server translation once");
            check(Binder.getCallingIdentity() == 10042L, "IPC failure lost caller identity");
            check(packages.identity == 0L, "Failed feature query inherited the external caller");
            check(!SystemFeaturesCache.hasInstance(), "Failed feature query published a cache");
            check(SystemFeaturesCache.constructions == 0 && SystemFeaturesCache.publications == 0,
                    "Failed feature query constructed or published cache state");
            check(ContextImpl.attempts == 0, "Failed feature query reached context construction");
            packages.failure = null;
        } else {
            packages.failure = new RemoteException("Existing cache must not query package-manager features");
            ActivityThread.packageManager = packages;
        }
        try { GesherSettingsContext.get(); throw new AssertionError("Unavailable service was hidden"); }
        catch (IllegalStateException expected) {
            check(true, "Failed construction remains retryable");
            check("Display service is not published".equals(expected.getMessage()),
                    "An earlier failure concealed the independent context failure");
        }
        check(Binder.getCallingIdentity() == 10042L, "Failure did not restore caller identity");
        SystemFeaturesCache initializedCache = SystemFeaturesCache.fixtureInstance();
        check(initializedCache != null, "Context attempt did not receive a feature cache");
        check(SystemFeaturesCache.constructions == 1 && SystemFeaturesCache.publications == 1,
                "Feature cache was not constructed and published exactly once");
        check(retained ? initializedCache == initialCache : initializedCache.receivedFeatures == packages.features,
                "Context lost the existing cache or exact package-manager catalog input");
        if (!retained) {
            check(packages.identity == 0L && ActivityThread.packageManagerLookupIdentity == 0L,
                    "Successful package lookup/query inherited the external caller");
            check(initializedCache.constructionIdentity == 0L && SystemFeaturesCache.publicationIdentity == 0L,
                    "Feature-cache construction/publication inherited the external caller");
        }
        int expectedLookups = retained ? 0 : 3;
        int expectedQueries = retained ? 0 : 2;
        check(ActivityThread.packageManagerLookups == expectedLookups,
                "Unexpected package-manager lookup count across initialization failures");
        check(packages.queries == expectedQueries, "Unexpected feature-query count across initialization failures");
        check(ContextImpl.attempts == 1 && ContextImpl.constructions == 0,
                "Context construction did not remain deferred until features were ready");
        check(ContextImpl.attemptIdentity == 0L, "Failed context attempt inherited the external caller");
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
        check(SystemFeaturesCache.fixtureInstance() == initializedCache,
                "Context retry or concurrent callers replaced the feature cache");
        check(SystemFeaturesCache.constructions == 1 && SystemFeaturesCache.publications == 1,
                "Concurrent callers reconstructed or republished the feature cache");
        check(ActivityThread.packageManagerLookups == expectedLookups && packages.queries == expectedQueries,
                "Context retry or cached calls queried features again");
        check(ContextImpl.attempts == 2, "Published context was constructed again");
        System.out.println("Settings context bootstrap contracts passed: " + checks
                + " checks; features=" + args[0]);
    }
}
