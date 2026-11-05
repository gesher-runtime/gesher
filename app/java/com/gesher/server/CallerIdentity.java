/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.Process;

/**
 * Authenticate an attached application's Binder PID, not its claimed package name.
 * Unknown remote applications fail closed. Local services and the host shell are
 * trusted control-plane callers; arbitrary execution as the Windows user is outside
 * this API isolation boundary.
 */
public final class CallerIdentity {
    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;

    public CallerIdentity(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm;
        mAtm = atm;
    }

    public int callerPid() { return Binder.getCallingPid(); }
    public int callerUid() { return Binder.getCallingUid(); }

    public String packageForPid(int pid) {
        return mAtm == null ? null : mAtm.authenticatedPackageForPid(pid);
    }

    public boolean isSystem() {
        int pid = callerPid();
        if (pid == Process.myPid()) return true;
        // An attached application cannot elevate itself by claiming a system package.
        if (packageForPid(pid) != null) return false;
        int uid = callerUid();
        return uid == Process.SYSTEM_UID || uid == Process.SHELL_UID;
    }

    public String callerPackage() {
        if (isSystem()) return "android";
        String name = packageForPid(callerPid());
        ApkPackage pkg = name == null ? null : mPm.get(name);
        if (pkg == null || (GmsSignatureSpoofing.isEnabled() && pkg.uid() != callerUid())) {
            throw new SecurityException("Unrecognized application Binder identity");
        }
        return name;
    }

    public String requirePackage(String claimedPackage) {
        if (isSystem()) return claimedPackage;
        String actual = callerPackage();
        if (!actual.equals(claimedPackage)) {
            throw new SecurityException("Caller cannot act for another package");
        }
        return actual;
    }

    public void requireSystemOrPackage(String ownerPackage) {
        if (!isSystem()) requirePackage(ownerPackage);
    }
}
