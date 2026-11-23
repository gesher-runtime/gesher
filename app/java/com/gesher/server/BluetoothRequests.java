/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.ServiceManager;

import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.IntConsumer;

/** User-consented adapter requests, with no Windows work on the activity/Binder thread. */
final class BluetoothRequests {
    private static final int MAX_PENDING = 16;
    private static final long TRANSITION_TIMEOUT_NS = 10_000_000_000L;

    /** Server-local boundaries for deterministic tests; never selected by an app or environment. */
    interface Backend {
        WindowsBluetooth.Adapter adapter();
        boolean confirm(String packageName, boolean enable);
        boolean setEnabled(boolean enable);
        default long nanoTime() { return System.nanoTime(); }
        default void pause() throws InterruptedException { Thread.sleep(50); }
    }

    private static final Backend WINDOWS = new Backend() {
        @Override public WindowsBluetooth.Adapter adapter() { return WindowsBluetooth.adapter(0); }
        @Override public boolean confirm(String packageName, boolean enable) {
            return PermissionRequests.confirm(packageName,
                    enable ? "Turn Windows Bluetooth on" : "Turn Windows Bluetooth off");
        }
        @Override public boolean setEnabled(boolean enable) {
            IBinder service = ServiceManager.getService(BluetoothManagerService.NAME);
            if (service instanceof BluetoothManagerService manager) return manager.setEnabled(enable);
            return WindowsBluetooth.setEnabled(enable) >= 0;
        }
    };

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Backend mBackend;
    private final Set<String> mPending = new HashSet<>();

    BluetoothRequests(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(work -> {
            Thread thread = new Thread(work, "bluetooth-request");
            thread.setDaemon(true);
            return thread;
        }), WINDOWS);
    }

    BluetoothRequests(PackageManagerService pm, Executor executor, Backend backend) {
        mPm = pm;
        mExecutor = executor;
        mBackend = backend;
    }

    static boolean handles(Intent intent, String resolvedType) {
        if (intent == null || intent.getComponent() != null) return false;
        Intent target = intent.getSelector() != null ? intent.getSelector() : intent;
        if (target.getComponent() != null || target.getData() != null || target.getType() != null
                || resolvedType != null) return false;
        String pkg = target.getPackage();
        if (pkg != null && !"com.android.settings".equals(pkg)
                && !"com.android.bluetooth".equals(pkg)) return false;
        Set<String> categories = target.getCategories();
        if (categories != null && (categories.size() > 1
                || !categories.contains(Intent.CATEGORY_DEFAULT))) return false;
        return isAction(target.getAction());
    }

    private static boolean isAction(String action) {
        return BluetoothAdapter.ACTION_REQUEST_ENABLE.equals(action)
                || BluetoothAdapter.ACTION_REQUEST_DISABLE.equals(action)
                || BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE.equals(action);
    }

    private String permission(String action, ApkPackage captured) {
        if (captured.parsed.getTargetSdkVersion() < 31) return "android.permission.BLUETOOTH";
        return BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE.equals(action)
                ? "android.permission.BLUETOOTH_ADVERTISE" : "android.permission.BLUETOOTH_CONNECT";
    }

    private boolean current(ApkPackage captured, BooleanSupplier lifetime) {
        return lifetime.getAsBoolean() && (captured == null
                || mPm.get(captured.packageName) == captured
                    && !mPm.isRemovalPending(captured.packageName)
                    && captured.matchesEnabledApplication(0));
    }

    private void enforce(String action, String packageName, ApkPackage captured) {
        if (captured == null) {
            if ("android".equals(packageName)) return; // ATMS authenticated a system caller.
            throw new SecurityException("Bluetooth request requires an installed caller");
        }
        if (!captured.packageName.equals(packageName)) {
            throw new SecurityException("Bluetooth request caller mismatch");
        }
        String permission = permission(action, captured);
        if (mPm.permissions().checkPermission(packageName, permission)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Need " + permission + " permission for " + packageName);
        }
    }

    void validate(String action, String packageName, ApkPackage captured) {
        // A consent activity does not bypass the caller's Android permission. This path
        // intentionally allows API 33+ apps to ask the user, unlike direct enable().
        enforce(action, packageName, captured);
    }

    void request(String action, String packageName, ApkPackage captured,
            BooleanSupplier lifetime, IntConsumer completion) {
        request(action, packageName, captured, lifetime, () -> { }, completion);
    }

    void request(String action, String packageName, ApkPackage captured,
            BooleanSupplier lifetime, Runnable beforeWork, IntConsumer completion) {
        validate(action, packageName, captured);
        boolean accepted;
        synchronized (mPending) {
            accepted = mPending.size() < MAX_PENDING && mPending.add(packageName);
        }
        if (!accepted) {
            completion.accept(Activity.RESULT_CANCELED);
            return;
        }
        AtomicBoolean started = new AtomicBoolean();
        try {
            mExecutor.execute(() -> {
                started.set(true);
                int result = Activity.RESULT_CANCELED;
                try {
                    beforeWork.run();
                    if (!Thread.currentThread().isInterrupted() && current(captured, lifetime)) {
                        enforce(action, packageName, captured);
                        // The host currently implements power and inquiry, not making this
                        // radio discoverable. Never substitute power-on for discoverability.
                        // A selector may resolve here with an unknown outer action. Settings
                        // cancels that delivered intent instead of executing the selector.
                        if (isAction(action)
                                && !BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE.equals(action)) {
                            boolean enable = BluetoothAdapter.ACTION_REQUEST_ENABLE.equals(action);
                            WindowsBluetooth.Adapter before = mBackend.adapter();
                            if (before.present && current(captured, lifetime)) {
                                enforce(action, packageName, captured);
                                if (before.on == enable) {
                                    result = Activity.RESULT_OK;
                                } else if (mBackend.confirm(packageName, enable)
                                        && current(captured, lifetime)) {
                                    // Grants/package identity can change while a dialog is open.
                                    enforce(action, packageName, captured);
                                    if (mBackend.setEnabled(enable)) {
                                        long began = mBackend.nanoTime();
                                        while (current(captured, lifetime)) {
                                            enforce(action, packageName, captured);
                                            WindowsBluetooth.Adapter after = mBackend.adapter();
                                            if (!after.present || !current(captured, lifetime)) break;
                                            enforce(action, packageName, captured);
                                            if (after.on == enable) {
                                                result = Activity.RESULT_OK;
                                                break;
                                            }
                                            if (mBackend.nanoTime() - began >= TRANSITION_TIMEOUT_NS) break;
                                            mBackend.pause();
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } catch (RuntimeException | LinkageError failure) {
                    SystemServer.log("bluetooth request failed: " + failure);
                } finally {
                    synchronized (mPending) { mPending.remove(packageName); }
                    completion.accept(result);
                }
            });
        } catch (RuntimeException rejected) {
            if (started.get()) throw rejected; // a direct test executor's completion failed
            synchronized (mPending) { mPending.remove(packageName); }
            completion.accept(Activity.RESULT_CANCELED);
        }
    }
}
