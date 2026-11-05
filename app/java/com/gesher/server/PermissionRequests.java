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
 */
final class PermissionRequests {
    public interface Decision { boolean allow(String packageName, String permissionNames); }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Decision mDecision;
    private final Set<String> mPending = new HashSet<>();

    PermissionRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(r -> {
            Thread thread = new Thread(r, "permission-consent");
            thread.setDaemon(true);
            return thread;
        }), PermissionRequests::confirm);
    }

    /** Server-local injection for regression tests; never exposed through Binder or env. */
    PermissionRequests(PackageManagerService pm, Executor executor, Decision decision) {
        mPm = pm;
        mExecutor = executor;
        mDecision = decision;
    }

    private static boolean confirm(String packageName, String permissions) {
        try {
            AccountSecretStore.ensureLoaded();
            return nativeConfirm(packageName, permissions);
        } catch (LinkageError | SecurityException e) {
            SystemServer.log("permission consent unavailable: " + e.getClass().getSimpleName());
            return false;
        }
    }

    private static native boolean nativeConfirm(String packageLabel, String permissionNames);

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
                    List<String> prompt = new ArrayList<>();
                    for (String name : names) {
                        if (name != null && policy.canRequest(captured.packageName, name)
                                && policy.checkPermission(captured.packageName, name)
                                        != PackageManager.PERMISSION_GRANTED
                                && !prompt.contains(name)) prompt.add(name);
                    }
                    boolean allow = !prompt.isEmpty() && mPm.get(captured.packageName) == captured
                            && mDecision.allow(captured.packageName, describe(policy, prompt));
                    // PM.add/remove are synchronized on the PM. Keep identity stable
                    // across the final recheck and the entire grant operation.
                    synchronized (mPm) {
                        if (mPm.get(captured.packageName) == captured) {
                            if (allow) {
                                for (String name : prompt) {
                                    if (policy.canRequest(captured.packageName, name)) {
                                        policy.grantRuntime(captured.packageName, name, 0);
                                    }
                                }
                            }
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
            if (text.length() != 0) text.append('\n');
            if (label != null) text.append(label).append(" (").append(name).append(")");
            else text.append(name);
        }
        return text.toString();
    }
}
