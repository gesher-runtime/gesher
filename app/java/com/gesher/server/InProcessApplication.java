/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IApplicationThread;
import android.content.Intent;
import android.os.Binder;
import android.os.Process;
import java.util.function.BooleanSupplier;

/**
 * The standalone launcher's admission, owned by the runtime before APK code is loaded.
 * The APK object accepted by PackageManager, not an app-supplied callingPackage string,
 * selects the identity. This is not a Binder API.
 */
public final class InProcessApplication implements AutoCloseable {
    private final ApkPackage mOwner;
    private final PackageManagerService mPm;
    private Services.Created mServices;
    private ActivityTaskManagerService.ProcessRecord mProcess;
    private AutoCloseable mChanges;
    private AutoCloseable mRemovals;
    private boolean mClosed;

    InProcessApplication(ApkPackage owner, PackageManagerService pm) {
        mOwner = owner;
        mPm = pm;
        requireCurrent();
    }

    private void requireCurrent() {
        if (mClosed || mPm.get(mOwner.packageName) != mOwner
                || mPm.isRemovalPending(mOwner.packageName) || mOwner.uidPending()
                || mOwner.uid() < Process.FIRST_APPLICATION_UID) {
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

    /** Called by the framework driver, before LoadedApk can load any application classes. */
    public synchronized void bind(IApplicationThread thread) {
        requireCurrent();
        if (mServices == null || mProcess != null || needsRestart()) {
            throw new IllegalStateException("Standalone process was not prepared for binding");
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
        synchronized (this) {
            if (mClosed) return;
            mClosed = true;
            services = mServices; process = mProcess;
            changes = mChanges; removals = mRemovals;
        }
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
