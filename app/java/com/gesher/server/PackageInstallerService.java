/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.IntentSender;
import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageInstallerCallback;
import android.content.pm.IPackageInstallerSession;
import android.content.pm.PackageInstaller;
import android.content.pm.ParceledListSlice;
import android.graphics.Bitmap;

import java.util.Collections;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Minimal stand-in implementation of {@link IPackageInstaller} so app stores
 * (such as Aptoide) can initialize their PackageInstaller without NullPointerExceptions.
 */
public final class PackageInstallerService extends IPackageInstaller.Default {
    private final AtomicInteger mNextSessionId = new AtomicInteger(1);

    @Override
    public int createSession(PackageInstaller.SessionParams params, String installerPackageName,
            String installerAttributionTag, int userId) {
        return mNextSessionId.getAndIncrement();
    }

    @Override
    public void updateSessionAppIcon(int sessionId, Bitmap appIcon) {
    }

    @Override
    public void updateSessionAppLabel(int sessionId, String appLabel) {
    }

    @Override
    public void abandonSession(int sessionId) {
    }

    @Override
    public IPackageInstallerSession openSession(int sessionId) {
        return null;
    }

    @Override
    public PackageInstaller.SessionInfo getSessionInfo(int sessionId) {
        return null;
    }

    @Override
    public ParceledListSlice<PackageInstaller.SessionInfo> getAllSessions(int userId) {
        return new ParceledListSlice<>(Collections.emptyList());
    }

    @Override
    public ParceledListSlice<PackageInstaller.SessionInfo> getMySessions(String installerPackageName, int userId) {
        return new ParceledListSlice<>(Collections.emptyList());
    }

    @Override
    public ParceledListSlice<PackageInstaller.SessionInfo> getStagedSessions() {
        return new ParceledListSlice<>(Collections.emptyList());
    }

    @Override
    public void registerCallback(IPackageInstallerCallback callback, int userId) {
    }

    @Override
    public void unregisterCallback(IPackageInstallerCallback callback) {
    }

    @Override
    public void installExistingPackage(String packageName, int installFlags, int installReason,
            IntentSender statusReceiver, int userId, java.util.List<String> whiteListedPermissions) {
    }

    @Override
    public void uninstall(android.content.pm.VersionedPackage versionedPackage, String callerPackageName,
            int flags, IntentSender statusReceiver, int userId) {
    }
}
