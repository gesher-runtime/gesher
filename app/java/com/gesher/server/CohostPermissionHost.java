/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageManager;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Permission checks and host consent for one admitted standalone code and live process. */
final class CohostPermissionHost implements AutoCloseable {
    static final int CHECK = 1;
    static final int REQUEST = 2;
    static final int FLAGS = 3;
    static final int CAN_REQUEST = 4;
    static final int APP_OP_MODE = 5;

    private final PermissionPolicy.ScopedOwner mOwner;
    private final PermissionRequests mRequests;
    private volatile long mLease;
    private boolean mCancelled;

    CohostPermissionHost(PackageManagerService pm, PermissionRequests requests, String apk)
            throws Exception {
        mRequests = requests;
        mOwner = pm.permissions().openScopedOwner(apk, this::isLive);
    }

    ApkPackage code() { return mOwner.code(); }
    Zygote.StandaloneInput input() { return mOwner.input(); }

    int runApplication(String runtimeOptions, String dataDir, List<String> options)
            throws Exception {
        return runApplication(runtimeOptions, dataDir, options, null);
    }

    int runApplication(String runtimeOptions, String dataDir, List<String> options, int[] streams)
            throws Exception {
        ApkPackage code = code();
        if (dataDir != null) code.setDataDir(dataDir);
        // The layout journal belongs to the host. Only the two actual data
        // trees take part in the private storage broker.
        code.prepareDataDirectories();
        AppSandbox.prepareStandaloneRuntime();
        String profile = "Gesher.Cohost." + UUID.randomUUID();
        if (Zygote.nativeEnsureAppContainer(profile, code.packageName) != 0) {
            throw new IllegalStateException("Cannot create the standalone AppContainer");
        }
        try {
            if (Zygote.nativeGrantPropertyAreaRead(profile) != 0) {
                throw new IllegalStateException("Cannot read the host system properties");
            }
            List<String> capabilities = new ArrayList<>(List.of(AppSandbox.RUNTIME_FILES));
            if (ConnectivityService.requests(code, ConnectivityService.INTERNET)) {
                capabilities.add(AppSandbox.INTERNET_CLIENT);
                capabilities.add(AppSandbox.PRIVATE_NETWORK);
            }
            List<String> arguments = new ArrayList<>(options);
            if (dataDir != null) {
                arguments.add("--data-dir");
                arguments.add(code.credentialProtectedDataDir());
            }
            arguments.add(code.apkPath);
            String command = Zygote.commandLine(runtimeOptions,
                    com.gesher.app.Main.class.getName(), arguments);
            return input().runCohost(command, profile, code.packageName,
                    capabilities.toArray(String[]::new), code.uid(), this,
                    Zygote.windowsPath(code.credentialProtectedDataDir()),
                    Zygote.windowsPath(code.deviceProtectedDataDir()), streams);
        } finally {
            Zygote.nativeDeleteAppContainer(profile);
        }
    }

    // The native launcher binds only the kernel process it actually created,
    // after admission and before ResumeThread. No guest supplies this value.
    private synchronized void bindNativeLease(long lease) {
        if (lease <= 0 || mLease != 0) throw new IllegalStateException("Already bound");
        mLease = lease;
        if (mCancelled) {
            Zygote.nativeCancelCohost(lease);
            throw new SecurityException("The standalone launcher already disconnected");
        }
    }

    private boolean isLive() {
        long lease = mLease;
        return lease != 0 && Zygote.nativeCohostPermissionLive(lease);
    }

    /** Invoked only by the pinned Windows transport. */
    private int[] dispatch(int operation, String[] names) throws Exception {
        if (!mOwner.isCurrent()) throw new SecurityException("Standalone permission owner retired");
        switch (operation) {
            case CHECK: return new int[] {mOwner.check(names[0])};
            case FLAGS: return new int[] {mOwner.flags(names[0])};
            case CAN_REQUEST: return new int[] {mOwner.canRequest(names[0]) ? 1 : 0};
            case APP_OP_MODE: return new int[] {mOwner.mode(names[0])};
            case REQUEST: {
                CompletableFuture<int[]> reply = new CompletableFuture<>();
                mRequests.requestScoped(mOwner, names, intent -> reply.complete(intent.getIntArrayExtra(
                        PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS)));
                while (mOwner.isCurrent()) {
                    try {
                        return reply.get(100, TimeUnit.MILLISECONDS);
                    } catch (TimeoutException pending) {
                        // PermissionRequests keeps the same native default-No
                        // consent policy as an installed app.
                    }
                }
                throw new SecurityException("Standalone permission owner retired");
            }
            default: throw new IllegalArgumentException("Invalid standalone permission operation");
        }
    }

    @Override public void close() {
        mOwner.close();
    }

    synchronized void cancel() {
        mCancelled = true;
        long lease = mLease;
        if (lease > 0) Zygote.nativeCancelCohost(lease);
    }
}
