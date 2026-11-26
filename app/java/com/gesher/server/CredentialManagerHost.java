/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Binder;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * {@link CredentialManagerService}'s view of the system server (docs/passkeys.md): the
 * Binder caller, installed packages and their verified signing certificates, permissions,
 * and one daemon thread per ceremony, since each waits for the user in Windows' dialog.
 */
final class CredentialManagerHost implements CredentialManagerService.Host {
    private final ActivityManagerService mAm;
    private final PackageManagerService mPm;
    private final AtomicInteger mCeremonies = new AtomicInteger();

    CredentialManagerHost(ActivityManagerService am, PackageManagerService pm) {
        mAm = am;
        mPm = pm;
    }

    @Override
    public int callingUid() {
        return Binder.getCallingUid();
    }

    @Override
    public int callingPid() {
        return Binder.getCallingPid();
    }

    @Override
    public int packageUid(String packageName) {
        ApkPackage pkg = mPm.get(packageName);
        return pkg == null || mPm.isRemovalPending(packageName) ? -1 : pkg.uid();
    }

    @Override
    public boolean hasPermission(String permission, int pid, int uid) {
        return mAm.checkPermission(permission, pid, uid) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * The certificates of a fully verified APK signature only: the WebAuthn origin and the
     * asset-links check must never rest on an unverified or substituted signature.
     */
    @Override
    public byte[][] signingCertificates(String packageName) {
        ApkPackage pkg = mPm.get(packageName);
        Signature[] signatures = pkg == null ? null : pkg.verifiedSignatures();
        if (signatures == null || signatures.length == 0) return null;
        byte[][] certificates = new byte[signatures.length][];
        for (int i = 0; i < signatures.length; i++) certificates[i] = signatures[i].toByteArray();
        return certificates;
    }

    @Override
    public void execute(Runnable ceremony) {
        Thread thread = new Thread(ceremony, "credential-" + mCeremonies.incrementAndGet());
        thread.setDaemon(true);
        thread.start();
    }
}
