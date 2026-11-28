/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IApplicationThread;
import android.content.Intent;
import android.os.Binder;
import android.os.Process;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.function.BooleanSupplier;

/**
 * The standalone launcher's admission, owned by the runtime before APK code is loaded.
 * The APK object accepted by PackageManager, not an app-supplied callingPackage string,
 * selects the identity. This is not a Binder API.
 */
public final class InProcessApplication implements AutoCloseable {
    private final ApkPackage mOwner;
    private final PackageManagerService mPm;
    private final PermissionRequests mPermissionRequests;
    private Services.Created mServices;
    private ActivityTaskManagerService.ProcessRecord mProcess;
    private AutoCloseable mChanges;
    private AutoCloseable mRemovals;
    private CompletableFuture<Boolean> mReview;
    private boolean mClosed;

    InProcessApplication(ApkPackage owner, PackageManagerService pm) {
        this(owner, pm, new PermissionRequests(pm));
    }

    /** Runtime-local controller injection; not reachable through a Binder request. */
    InProcessApplication(ApkPackage owner, PackageManagerService pm, PermissionRequests requests) {
        mOwner = owner;
        mPm = pm;
        mPermissionRequests = java.util.Objects.requireNonNull(requests);
        requireCurrent();
    }

    private boolean current() {
        return !mClosed && mPm.get(mOwner.packageName) == mOwner
                && !mPm.isRemovalPending(mOwner.packageName) && !mOwner.uidPending()
                && mOwner.uid() >= Process.FIRST_APPLICATION_UID
                && mOwner.matchesEnabledApplication(0);
    }

    private void requireCurrent() {
        if (!current()) {
            throw new IllegalStateException("Standalone APK admission is no longer current");
        }
    }

    /** The UID is immutable in a VM; changing its environment now would leave stale identity. */
    public synchronized boolean needsRestart() {
        requireCurrent();
        String binderUid = System.getenv("GESHER_BINDER_UID");
        return Process.myUid() != mOwner.uid() || Binder.getCallingUid() != mOwner.uid()
                || Binder.getCallingPid() != Process.myPid()
                || binderUid != null && !binderUid.equals(Integer.toString(mOwner.uid()));
    }

    /** Re-enter the same launcher, under the admitted UID from process creation onward. */
    public int restart() {
        synchronized (this) {
            if (!needsRestart() || mServices != null) {
                throw new IllegalStateException("Standalone restart is not required before startup");
            }
            if ("1".equals(System.getenv("GESHER_STANDALONE_RESTARTED"))) {
                throw new IllegalStateException("Standalone VM identity does not match admitted APK");
            }
        }
        return Zygote.restartWithUid(mOwner.uid());
    }

    /** Install services only in the VM with the canonical identity, before application code. */
    public synchronized void install() throws ReflectiveOperationException {
        requireCurrent();
        if (mServices != null || needsRestart()) {
            throw new IllegalStateException("Standalone services require the admitted process identity");
        }
        mServices = Services.create(mPm, false);
        mChanges = mPm.onPackageChange(pkg -> {
            if (mOwner.packageName.equals(pkg.packageName) && pkg != mOwner) close();
        });
        mRemovals = mPm.onPackageRemoval(name -> {
            if (mOwner.packageName.equals(name)) close();
        });
        Services.installInProcess(mServices.services);
    }

    /**
     * The standalone launcher's only human wait, before any APK code or app window.
     * The shared permission controller owns the worker and commits explicit choices;
     * this admission owns whether its exact installed generation may proceed.
     */
    public boolean awaitPermissionReview() throws InterruptedException {
        final CompletableFuture<Boolean> result;
        final boolean start;
        synchronized (this) {
            requireCurrent();
            if (mServices == null || mProcess != null || needsRestart()) {
                throw new IllegalStateException("Standalone review requires prepared, unbound startup");
            }
            // The same-user broker reviews the pinned code before it admits this guest.
            if (mPm.isCohost()) return Zygote.nativeHasCohostPermissionScope();
            if (mReview == null && !mPm.permissions().isReviewRequired(mOwner)) return true;
            start = mReview == null;
            if (start) mReview = new CompletableFuture<>();
            result = mReview;
        }
        // Never invoke the controller or wait while holding an admission/service lock.
        if (start && !result.isDone()) {
            try {
                mPermissionRequests.reviewPermissions(mOwner).whenComplete((approved, failure) -> {
                    boolean proceed;
                    synchronized (this) {
                        proceed = failure == null && Boolean.TRUE.equals(approved) && current()
                                && !mPm.permissions().isReviewRequired(mOwner);
                    }
                    result.complete(proceed);
                });
            } catch (RuntimeException failure) {
                SystemServer.log("standalone permission review unavailable: " + failure.getClass().getSimpleName());
                result.complete(false);
            }
        }
        try {
            return result.get();
        } catch (InterruptedException interrupted) {
            result.complete(false);
            throw interrupted;
        } catch (ExecutionException failure) {
            throw new IllegalStateException("Standalone permission review failed", failure.getCause());
        }
    }

    public synchronized void prepareStorage() throws java.io.IOException {
        requireCurrent();
        if (!mPm.isCohost()) {
            mOwner.prepareDataDirectories();
            LegacyKeyStoreArchive.prepare(mOwner);
            return;
        }
        if (!Zygote.nativeCohostOwnsData(
                Zygote.windowsPath(mOwner.credentialProtectedDataDir()),
                Zygote.windowsPath(mOwner.deviceProtectedDataDir()))) {
            throw new SecurityException("The standalone data paths do not match their host admission");
        }
    }

    /** Called by the framework driver, before LoadedApk can load any application classes. */
    public synchronized void bind(IApplicationThread thread) {
        requireCurrent();
        if (mServices == null || mProcess != null || needsRestart()) {
            throw new IllegalStateException("Standalone process was not prepared for binding");
        }
        if (mReview != null && !Boolean.TRUE.equals(mReview.getNow(false))
                || mPm.permissions().isReviewRequired(mOwner)) {
            throw new IllegalStateException("Standalone permission review has not been confirmed");
        }
        mProcess = mServices.atm.attachInProcessApplication(mOwner, thread);
    }

    /** Result target registered by the trusted standalone driver. */
    @FunctionalInterface
    public interface ActivityResult {
        void dispatch(Binder token, String who, int requestCode, Intent data);
    }

    public synchronized Binder registerActivity(BooleanSupplier active, ActivityResult result) {
        requireCurrent();
        if (mProcess == null) throw new IllegalStateException("Standalone process was not bound");
        return mServices.atm.registerInProcessActivity(mProcess, active, result);
    }

    public void activityStarted() { activityState(ActivityTaskManagerService.State.STARTED); }
    public void activityResuming() { activityState(ActivityTaskManagerService.State.RESUMING); }
    public void activityResumed() { activityState(ActivityTaskManagerService.State.RESUMED); }
    public void activityPaused() { activityState(ActivityTaskManagerService.State.PAUSED); }
    public void activityStopped() { activityState(ActivityTaskManagerService.State.STOPPED); }

    private synchronized void activityState(ActivityTaskManagerService.State state) {
        if (!mClosed && mProcess != null) mServices.atm.inProcessActivityState(mProcess, state);
    }

    /** Retires both process ownership and listener/native resources on every terminal path. */
    @Override public void close() {
        Services.Created services;
        ActivityTaskManagerService.ProcessRecord process;
        AutoCloseable changes, removals;
        CompletableFuture<Boolean> review;
        synchronized (this) {
            if (mClosed) return;
            mClosed = true;
            services = mServices; process = mProcess;
            changes = mChanges; removals = mRemovals;
            review = mReview;
        }
        // Closing/replacing the admission must unblock startup even if a host
        // dialog is still awaiting its own user response on the consent worker.
        if (review != null) review.complete(false);
        try {
            if (process != null) services.atm.retireInProcessApplication(process);
        } finally {
            try {
                if (services != null) services.clipboard.close();
            } finally {
                try {
                    if (changes != null) changes.close();
                } catch (Exception failure) {
                    throw new IllegalStateException("Removing standalone package listener", failure);
                } finally {
                    try {
                        if (removals != null) removals.close();
                    } catch (Exception failure) {
                        throw new IllegalStateException("Removing standalone removal listener", failure);
                    }
                }
            }
        }
    }
}
