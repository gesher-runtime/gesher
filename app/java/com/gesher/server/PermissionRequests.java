/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.res.Resources;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.function.Consumer;

/**
 * The host runtime-permission controller. The ATM authenticates the requesting
 * activity/process before entering here; intent extras never identify the owner.
 * The host's default-No consent dialog runs away from app/Binder/UI threads.
 * A background permission (ACCESS_BACKGROUND_LOCATION) is never granted by the
 * foreground Allow: like Android 16's PermissionController it has its own choice,
 * here a separate native dialog (see PermissionPolicy.planRequest).
 */
final class PermissionRequests {
    // Keep these values in sync with compat/credentials/permission_dialog.inc and
    // background_permission_dialog.inc. Cancellation is not a user denial.
    static final int DISMISSED = 0, DENIED = 1, ALLOWED = 2, FOREGROUND_ONLY = 3, ALWAYS = 4;
    public interface Decision {
        /** The foreground dialog: DISMISSED, DENIED or ALLOWED. */
        int decide(String packageName, String permissionNames);
        /** The background dialog: DISMISSED, DENIED, FOREGROUND_ONLY or ALWAYS. */
        int decideBackground(String packageName, String permissionNames);
    }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Decision mDecision;
    private final Set<String> mPending = new HashSet<>();

    PermissionRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(r -> {
            Thread thread = new Thread(r, "permission-consent");
            thread.setDaemon(true);
            return thread;
        }), new Decision() {
            @Override public int decide(String packageName, String permissionNames) {
                return confirmDecision(packageName, permissionNames);
            }
            @Override public int decideBackground(String packageName, String permissionNames) {
                return confirmBackground(packageName, permissionNames);
            }
        });
    }

    /** Server-local injection for regression tests; never exposed through Binder or env. */
    PermissionRequests(PackageManagerService pm, Executor executor, Decision decision) {
        mPm = pm;
        mExecutor = executor;
        mDecision = decision;
    }

    /** Other host consent flows need only the explicit-Allow result. */
    static boolean confirm(String packageName, String permissions) {
        return confirmDecision(packageName, permissions) == ALLOWED;
    }

    static int confirmDecision(String packageName, String permissions) {
        try {
            AccountSecretStore.ensureLoaded();
            return nativeConfirm(packageName, permissions);
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("permission consent unavailable: " + e.getClass().getSimpleName());
            return DISMISSED;
        }
    }

    static int confirmBackground(String packageName, String permissions) {
        try {
            AccountSecretStore.ensureLoaded();
            int choice = nativeConfirmBackground(packageName, permissions);
            return choice == DENIED || choice == FOREGROUND_ONLY || choice == ALWAYS ? choice : DISMISSED;
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("background permission consent unavailable: " + e.getClass().getSimpleName());
            return DISMISSED;
        }
    }

    private static native int nativeConfirm(String packageLabel, String permissionNames);
    private static native int nativeConfirmBackground(String packageLabel, String permissionNames);

    private boolean current(ApkPackage captured) {
        return !mPm.isRemovalPending(captured.packageName) && mPm.get(captured.packageName) == captured;
    }

    /**
     * The background dialog, then its commit. The policy rechecks this APK generation and the
     * granted foreground under the PM monitor, and stops the app only after releasing it.
     */
    private void chooseBackground(PermissionPolicy policy, ApkPackage captured, String background,
            boolean killOnRevoke) {
        if (!current(captured) || !policy.backgroundChoiceAvailable(captured, background)) return;
        int choice = mDecision.decideBackground(captured.packageName, describe(policy, List.of(background)));
        if (choice != DENIED && choice != FOREGROUND_ONLY && choice != ALWAYS) return;
        policy.recordBackgroundChoice(captured, background, choice, killOnRevoke);
    }

    /**
     * An app opening its own App info (ACTION_APPLICATION_DETAILS_SETTINGS) while one of its
     * background permissions waits for the user's choice, as microG does after a request:
     * Android's Settings page offers that choice, so show the background dialog, then run
     * {@code completion} once the user has chosen. Returns false (nothing shown) when no
     * such choice is pending; the caller then opens the Control Center as before.
     */
    boolean backgroundSettings(ApkPackage captured, Runnable completion) {
        if (captured == null || completion == null) throw new IllegalArgumentException("Invalid settings request");
        PermissionPolicy policy = mPm.permissions();
        String background = policy.pendingBackgroundChoice(captured);
        if (background == null) return false;
        boolean accepted;
        synchronized (mPending) { accepted = mPending.add(captured.packageName); }
        if (!accepted) {
            // A consent dialog for this app is already open; Settings returns at once.
            completion.run();
            return true;
        }
        try {
            mExecutor.execute(() -> {
                try {
                    chooseBackground(policy, captured, background, true);
                } catch (RuntimeException e) {
                    SystemServer.log("background permission choice failed for " + captured.packageName
                            + ": " + e.getClass().getSimpleName());
                } finally {
                    synchronized (mPending) { mPending.remove(captured.packageName); }
                }
                completion.run();
            });
        } catch (RuntimeException e) {
            synchronized (mPending) { mPending.remove(captured.packageName); }
            throw e;
        }
        return true;
    }

    void request(ApkPackage captured, String[] requested, Consumer<Intent> completion) {
        if (captured == null || requested == null || requested.length > 128) {
            throw new IllegalArgumentException("Invalid runtime permission request");
        }
        String[] names = requested.clone();
        for (String name : names) {
            if (name != null && name.length() > 300) {
                throw new IllegalArgumentException("Invalid permission name");
            }
        }
        boolean accepted;
        synchronized (mPending) { accepted = mPending.add(captured.packageName); }
        if (!accepted) {
            int[] denied = new int[names.length];
            Arrays.fill(denied, PackageManager.PERMISSION_DENIED);
            completion.accept(result(names, denied));
            return;
        }
        try {
            mExecutor.execute(() -> {
                int[] grants = new int[names.length];
                Arrays.fill(grants, PackageManager.PERMISSION_DENIED);
                try {
                    PermissionPolicy policy = mPm.permissions();
                    PermissionPolicy.RequestPlan plan = current(captured)
                            ? policy.planRequest(captured, names) : PermissionPolicy.RequestPlan.NONE;
                    List<String> prompt = plan.foreground();
                    int decision = !prompt.isEmpty() && current(captured)
                            ? mDecision.decide(captured.packageName, describe(policy, prompt)) : DISMISSED;
                    // PM.add/remove are synchronized on the PM. Keep identity stable
                    // across the final recheck and the entire grant operation.
                    synchronized (mPm) {
                        if (current(captured) && (decision == ALLOWED || decision == DENIED)) {
                            policy.recordUserDecision(captured, prompt, decision == ALLOWED);
                        }
                    }
                    // Each background permission gets its own choice once its foreground is
                    // granted; a foreground denial leaves it unasked, as on Android.
                    for (String background : plan.background()) {
                        chooseBackground(policy, captured, background,
                                plan.foregroundAlreadyGranted().contains(background));
                    }
                    synchronized (mPm) {
                        if (current(captured)) {
                            for (int i = 0; i < names.length; i++) {
                                if (names[i] != null) {
                                    grants[i] = policy.checkPermission(captured.packageName, names[i]);
                                }
                            }
                        }
                    }
                } catch (RuntimeException e) {
                    SystemServer.log("permission request denied for " + captured.packageName
                            + ": " + e.getClass().getSimpleName());
                } finally {
                    synchronized (mPending) { mPending.remove(captured.packageName); }
                }
                completion.accept(result(names, grants));
            });
        } catch (RuntimeException e) {
            synchronized (mPending) { mPending.remove(captured.packageName); }
            throw e;
        }
    }

    private static Intent result(String[] names, int[] grants) {
        return new Intent().putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, names)
                .putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, grants)
                .putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_DEVICE_ID, 0);
    }

    private static String describe(PermissionPolicy policy, List<String> names) {
        StringBuilder text = new StringBuilder();
        for (String name : names) {
            PermissionInfo info = policy.getPermissionInfo(name, "android", 0);
            CharSequence label = null;
            if (info != null && "android".equals(info.packageName) && info.labelRes != 0) {
                try { label = Resources.getSystem().getText(info.labelRes); }
                catch (Resources.NotFoundException ignored) { }
            }
            if (text.length() != 0) text.append("\r\n");
            // The native host dialog uses GDI: use LRE/PDF rather than FSI/PDI.
            // Keep the technical identifier and its parentheses LTR inside RTL labels.
            if (label != null) text.append(label).append(" \u202a(").append(name).append(")\u202c");
            else text.append('\u202a').append(name).append('\u202c');
        }
        return text.toString();
    }
}
