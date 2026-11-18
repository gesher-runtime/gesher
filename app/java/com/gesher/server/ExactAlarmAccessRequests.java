/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.Intent;
import android.content.ComponentName;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Binder;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.Semaphore;
import java.util.function.IntConsumer;

/** Host presentation of Android's Alarms & reminders special-access Settings screen. */
final class ExactAlarmAccessRequests {
    static final String ACTION = "android.settings.REQUEST_SCHEDULE_EXACT_ALARM";
    static final String PACKAGE = "com.android.settings";
    static final String LIST_ACTIVITY = PACKAGE + ".Settings$AlarmsAndRemindersActivity";
    static final String APP_ACTIVITY = PACKAGE + ".Settings$AlarmsAndRemindersAppActivity";

    interface Dialog {
        /** Selected package index * 2 plus allowed (1/0), or -1 for dismissal. */
        int edit(String[] packages, boolean[] allowed);
    }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Dialog mDialog;
    private final boolean mNativeDialog;
    private final Semaphore mSlots = new Semaphore(16);

    ExactAlarmAccessRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(task -> {
            Thread thread = new Thread(task, "exact-alarm-settings");
            thread.setDaemon(true);
            return thread;
        }), ExactAlarmAccessRequests::show, true);
    }

    /** Tests replace only the host window; target snapshots and AppOps writes remain real. */
    ExactAlarmAccessRequests(PackageManagerService pm, Executor executor, Dialog dialog) {
        this(pm, executor, dialog, false);
    }

    private ExactAlarmAccessRequests(PackageManagerService pm, Executor executor, Dialog dialog,
            boolean nativeDialog) {
        mPm = pm;
        mExecutor = executor;
        mDialog = dialog;
        mNativeDialog = nativeDialog;
    }

    private static String activity(Intent intent, String resolvedType) {
        if (intent == null) return null;
        intent = resolutionIntent(intent);
        ComponentName component = intent.getComponent();
        if (component != null) return PACKAGE.equals(component.getPackageName())
                && (LIST_ACTIVITY.equals(component.getClassName()) || APP_ACTIVITY.equals(component.getClassName()))
                        ? component.getClassName() : null;
        if (!SettingsIntents.addressesSettings(intent) || !ACTION.equals(intent.getAction())
                || resolvedType != null || intent.getType() != null) return null;
        if (intent.getCategories() != null) for (String category : intent.getCategories()) {
            if (!Intent.CATEGORY_DEFAULT.equals(category)) return null;
        }
        Uri data = intent.getData();
        return data == null ? LIST_ACTIVITY : "package".equals(data.getScheme()) ? APP_ACTIVITY : null;
    }

    static boolean handles(Intent intent, String resolvedType) {
        return activity(intent, resolvedType) != null;
    }

    private static Intent resolutionIntent(Intent intent) {
        return intent.getComponent() == null && intent.getSelector() != null ? intent.getSelector() : intent;
    }

    static boolean isActivity(ActivityInfo info) {
        return info != null && PACKAGE.equals(info.packageName)
                && (LIST_ACTIVITY.equals(info.name) || APP_ACTIVITY.equals(info.name));
    }

    static String target(Intent intent) {
        intent = resolutionIntent(intent);
        if (LIST_ACTIVITY.equals(activity(intent, intent.getType()))) return null;
        Uri data = intent.getData();
        // An explicitly addressed app-details activity with no valid package finishes,
        // as Settings does; it must not silently become a device-wide permission list.
        return data != null && "package".equals(data.getScheme()) && data.getSchemeSpecificPart() != null
                ? data.getSchemeSpecificPart() : "";
    }

    ActivityInfo activityInfo(ComponentName component, long flags) {
        if (component == null || !PACKAGE.equals(component.getPackageName())
                || !(LIST_ACTIVITY.equals(component.getClassName()) || APP_ACTIVITY.equals(component.getClassName()))
                || mPm.get(PACKAGE) != null || !available()) return null;
        long boot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        if (boot != 0 && (boot & PackageManager.MATCH_DIRECT_BOOT_AWARE) == 0) return null;
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = PACKAGE; app.uid = android.os.Process.SYSTEM_UID;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        app.enabled = true; app.targetSdkVersion = android.os.Build.VERSION.SDK_INT;
        ActivityInfo info = new ActivityInfo();
        info.packageName = PACKAGE; info.name = component.getClassName(); info.applicationInfo = app;
        info.exported = true; info.enabled = true; info.processName = "system_server";
        info.directBootAware = true; // Inherited from Settings' application manifest.
        info.nonLocalizedLabel = "Alarms & reminders";
        return info;
    }

    ResolveInfo resolve(Intent intent, String resolvedType, long flags) {
        String activity = activity(intent, resolvedType);
        if (activity == null) return null;
        intent = resolutionIntent(intent);
        ActivityInfo info = activityInfo(new ComponentName(PACKAGE, activity), flags);
        if (info == null) return null;
        ResolveInfo result = new ResolveInfo(); result.activityInfo = info;
        result.userHandle = android.os.UserHandle.SYSTEM;
        if (intent.getComponent() != null) return result;
        result.priority = 1; result.isDefault = true;
        result.system = true;
        result.match = (intent.getData() == null ? IntentFilter.MATCH_CATEGORY_EMPTY : IntentFilter.MATCH_CATEGORY_SCHEME)
                + IntentFilter.MATCH_ADJUSTMENT_NORMAL;
        if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
            result.filter = new IntentFilter(ACTION);
            result.filter.addCategory(Intent.CATEGORY_DEFAULT);
            result.filter.setPriority(1);
            if (APP_ACTIVITY.equals(activity)) result.filter.addDataScheme("package");
        }
        return result;
    }

    private static native int nativeEdit(String[] packages, boolean[] allowed);

    boolean available() {
        if (!mNativeDialog) return true;
        try { AccountSecretStore.ensureLoaded(); return true; }
        catch (LinkageError | SecurityException failure) { return false; }
    }

    private static int show(String[] packages, boolean[] allowed) {
        AccountSecretStore.ensureLoaded();
        return nativeEdit(packages, allowed);
    }

    boolean request(String packageName, Runnable beforeOpen, IntConsumer completed) {
        List<ApkPackage> packages = new ArrayList<>();
        for (ApkPackage pkg : mPm.packages()) {
            if ((packageName == null || packageName.equals(pkg.packageName))
                    && !mPm.isRemovalPending(pkg.packageName) && ExactAlarmAccess.editable(mPm, pkg)) {
                packages.add(pkg);
            }
        }
        packages.sort(Comparator.comparing(pkg -> pkg.packageName));
        if (!mSlots.tryAcquire()) return false;
        java.util.concurrent.atomic.AtomicBoolean slotReleased = new java.util.concurrent.atomic.AtomicBoolean();
        Runnable releaseSlot = () -> {
            if (slotReleased.compareAndSet(false, true)) mSlots.release();
        };
        try { mExecutor.execute(() -> {
            long identity = 0;
            boolean cleared = false;
            int activityResult = android.app.Activity.RESULT_CANCELED;
            try {
                identity = Binder.clearCallingIdentity();
                cleared = true;
                beforeOpen.run();
                // A specific, ineligible package's Settings detail finishes without a change.
                if (packageName != null && packages.isEmpty()) return;
                String[] names = new String[packages.size()];
                boolean[] allowed = new boolean[packages.size()];
                for (int i = 0; i < packages.size(); i++) {
                    ApkPackage pkg = packages.get(i);
                    if (mPm.get(pkg.packageName) != pkg || mPm.isRemovalPending(pkg.packageName)) return;
                    names[i] = pkg.packageName;
                    allowed[i] = ExactAlarmAccess.hasSchedule(mPm, pkg);
                }
                int result = mDialog.edit(names, allowed);
                if (result < 0 || result / 2 >= packages.size()) return;
                ApkPackage selected = packages.get(result / 2);
                if (!ExactAlarmAccess.editable(mPm, selected)) return;
                // The AppOps service commits only to this installed generation and
                // dispatches callbacks after releasing the package/state locks.
                boolean granted = (result & 1) != 0;
                boolean committed = mPm.appOps().setUidModeIfCurrent(AppOpsManager.OP_SCHEDULE_EXACT_ALARM,
                        selected, granted ? AppOpsManager.MODE_ALLOWED : AppOpsManager.MODE_ERRORED);
                if (committed && granted && packageName != null) activityResult = android.app.Activity.RESULT_OK;
            } catch (RuntimeException | LinkageError failure) {
                SystemServer.log("exact alarm settings: " + failure.getClass().getSimpleName());
            } finally {
                // No completion runs while the native window/JNI call is still open.
                // Failure to restore identity must not strand the accepted session.
                try { if (cleared) Binder.restoreCallingIdentity(identity); }
                finally {
                    try { completed.accept(activityResult); }
                    finally { releaseSlot.run(); }
                }
            }
        }); } catch (RuntimeException | Error rejected) {
            // An inline executor can propagate a worker failure after its finally
            // already released the slot; a rejected submission has no worker finally.
            releaseSlot.run();
            throw rejected;
        }
        return true;
    }
}
