/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.Binder;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.Semaphore;
import java.util.function.IntConsumer;

/** Real legacy administrator enrollment, presented by the trusted host Settings UI. */
final class DeviceAdminRequests {
    static final String ACTION = "android.app.action.ADD_DEVICE_ADMIN";
    static final String PACKAGE = "com.android.settings";
    static final String ACTIVITY = PACKAGE + ".DeviceAdminAdd";
    private final PackageManagerService pm;
    private final Executor executor;
    private final Dialog dialog;
    private final boolean nativeDialog;
    private final Semaphore slots = new Semaphore(16);
    interface Dialog { boolean confirm(String label, String description, boolean supported); }
    DeviceAdminRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(task -> {
            Thread t = new Thread(task, "device-admin-settings"); t.setDaemon(true); return t;
        }), DeviceAdminRequests::show, true);
    }
    DeviceAdminRequests(PackageManagerService pm, Executor executor, Dialog dialog) {
        this(pm, executor, dialog, false);
    }
    private DeviceAdminRequests(PackageManagerService pm, Executor executor, Dialog dialog, boolean nativeDialog) {
        this.pm = pm; this.executor = executor; this.dialog = dialog; this.nativeDialog = nativeDialog;
    }
    private static Intent resolutionIntent(Intent intent) {
        return intent.getComponent() == null && intent.getSelector() != null ? intent.getSelector() : intent;
    }
    static boolean handles(Intent intent, String type) {
        if (intent == null) return false;
        intent = resolutionIntent(intent);
        ComponentName c = intent.getComponent();
        if (c != null) return PACKAGE.equals(c.getPackageName()) && ACTIVITY.equals(c.getClassName());
        if (!SettingsIntents.addressesSettings(intent) || !ACTION.equals(intent.getAction())
                || type != null || intent.getType() != null || intent.getData() != null) return false;
        if (intent.getCategories() != null) for (String category : intent.getCategories()) {
            if (!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        }
        return true;
    }
    static boolean isActivity(ActivityInfo info) {
        return info != null && PACKAGE.equals(info.packageName) && ACTIVITY.equals(info.name);
    }
    boolean available() {
        if (!nativeDialog) return true;
        try { AccountSecretStore.ensureLoaded(); return nativeAvailable(); }
        catch (LinkageError | SecurityException failure) { return false; }
    }
    private static native boolean nativeAvailable();
    private static native boolean nativeConfirm(String label, String description, boolean supported);
    private static boolean show(String label, String description, boolean supported) {
        AccountSecretStore.ensureLoaded(); return nativeConfirm(label, description, supported);
    }
    ActivityInfo activityInfo(ComponentName component, long flags) {
        if (component == null || !PACKAGE.equals(component.getPackageName()) || !ACTIVITY.equals(component.getClassName())
                || pm.get(PACKAGE) != null || !available()) return null;
        long boot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        if (boot != 0 && (boot & PackageManager.MATCH_DIRECT_BOOT_AWARE) == 0) return null;
        ApplicationInfo app = new ApplicationInfo();
        app.packageName = PACKAGE; app.uid = android.os.Process.SYSTEM_UID;
        app.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        app.enabled = true; app.targetSdkVersion = android.os.Build.VERSION.SDK_INT;
        ActivityInfo info = new ActivityInfo();
        info.packageName = PACKAGE; info.name = ACTIVITY; info.applicationInfo = app;
        info.exported = true; info.enabled = true; info.processName = "system_server";
        info.directBootAware = true; info.nonLocalizedLabel = "Device administration";
        return info;
    }
    ResolveInfo resolve(Intent intent, String type, long flags) {
        if (!handles(intent, type)) return null;
        ActivityInfo info = activityInfo(new ComponentName(PACKAGE, ACTIVITY), flags);
        if (info == null) return null;
        ResolveInfo result = new ResolveInfo(); result.activityInfo = info;
        result.userHandle = android.os.UserHandle.SYSTEM;
        if (resolutionIntent(intent).getComponent() != null) return result;
        result.priority = 1; result.isDefault = true; result.system = true;
        result.match = IntentFilter.MATCH_CATEGORY_EMPTY + IntentFilter.MATCH_ADJUSTMENT_NORMAL;
        if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
            result.filter = new IntentFilter(ACTION); result.filter.addCategory(Intent.CATEGORY_DEFAULT);
            result.filter.setPriority(1);
        }
        return result;
    }
    boolean request(Intent intent, Runnable beforeOpen, IntConsumer completed) {
        final ComponentName admin;
        final String explanation;
        try {
            // A selector chooses the target, but Activity receives the original extras.
            admin = intent.getParcelableExtra("android.app.extra.DEVICE_ADMIN", ComponentName.class);
            CharSequence supplied = intent.getCharSequenceExtra("android.app.extra.ADD_EXPLANATION");
            String plain = supplied == null ? "" : supplied.toString();
            int limit = Math.min(plain.length(), 4096);
            if (limit < plain.length() && limit > 0 && Character.isHighSurrogate(plain.charAt(limit - 1))) --limit;
            explanation = plain.substring(0, limit) + (limit < plain.length() ? "\u2026" : "");
        }
        catch (RuntimeException invalid) { completed.accept(Activity.RESULT_CANCELED); return true; }
        DeviceAdminPolicyService.Request request = pm.deviceAdminPolicy().prepare(admin);
        if (!slots.tryAcquire()) return false;
        java.util.concurrent.atomic.AtomicBoolean released = new java.util.concurrent.atomic.AtomicBoolean();
        Runnable release = () -> { if (released.compareAndSet(false, true)) slots.release(); };
        try { executor.execute(() -> {
            int result = Activity.RESULT_CANCELED;
            long identity = 0;
            boolean cleared = false;
            try {
                identity = Binder.clearCallingIdentity();
                cleared = true;
                beforeOpen.run();
                if (request != null) {
                    if (request.supported && pm.deviceAdminPolicy().isAdminActive(admin, 0)) result = Activity.RESULT_OK;
                    else if (dialog.confirm(request.label + "\n" + request.admin.flattenToShortString(),
                            request.description + (explanation.isEmpty() ? "" : "\n\n" + explanation), request.supported)
                            && request.supported && pm.deviceAdminPolicy().activate(request)) result = Activity.RESULT_OK;
                }
            } catch (RuntimeException | LinkageError failure) {
                SystemServer.log("device admin settings: " + failure.getClass().getSimpleName());
            } finally {
                try { if (cleared) Binder.restoreCallingIdentity(identity); }
                finally { try { completed.accept(result); } finally { release.run(); } }
            }
        }); } catch (RuntimeException | Error rejected) { release.run(); throw rejected; }
        return true;
    }
}
