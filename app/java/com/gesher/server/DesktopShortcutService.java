/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;

import java.util.List;
import java.util.Objects;

/**
 * Windows has no Android shortcut host, share-target index, or pinned-item UI.
 * Queries describe that absence; publishing fails explicitly rather than
 * reporting throttling or pretending that a shortcut was created. Pin requests
 * return false, for which Android specifies that no result callback is sent.
 */
class DesktopShortcutService extends IShortcutService.Default {
    private final PackageManagerService mPackages;

    DesktopShortcutService(PackageManagerService packages) { mPackages = packages; }

    private void query(String packageName, int userId) {
        if (userId != 0) throw new IllegalArgumentException("Unknown user " + userId);
        Objects.requireNonNull(packageName, "packageName");
        if (mPackages.getPackageInfo(packageName, 0, userId) == null) {
            throw new IllegalArgumentException("Unknown package " + packageName);
        }
    }

    private static UnsupportedOperationException publishingUnavailable() {
        return new UnsupportedOperationException("This Windows environment has no Android "
                + "shortcut publisher or launcher integration");
    }

    @Override public boolean isRequestPinItemSupported(int userId, int requestType) { return false; }
    @Override public boolean requestPinShortcut(String pkg, ShortcutInfo info,
            IntentSender result, int userId) {
        query(pkg, userId); Objects.requireNonNull(info, "shortcut"); return false;
    }
    @Override public Intent createShortcutResultIntent(String pkg, ShortcutInfo info, int userId) {
        query(pkg, userId); Objects.requireNonNull(info, "shortcut"); return null;
    }
    @Override public ParceledListSlice getShortcuts(String pkg, int flags, int userId) {
        query(pkg, userId); return ParceledListSlice.emptyList();
    }
    @Override public ParceledListSlice getShareTargets(String pkg, IntentFilter filter, int userId) {
        query(pkg, userId); Objects.requireNonNull(filter, "filter");
        return ParceledListSlice.emptyList();
    }
    @Override public boolean hasShareTargets(String caller, String packageName, int userId) {
        query(caller, userId); return false;
    }
    @Override public int getMaxShortcutCountPerActivity(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public int getRemainingCallCount(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public long getRateLimitResetTime(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public int getIconMaxDimensions(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public boolean setDynamicShortcuts(String pkg, ParceledListSlice list, int userId) {
        query(pkg, userId); throw publishingUnavailable();
    }
    @Override public boolean addDynamicShortcuts(String pkg, ParceledListSlice list, int userId) {
        query(pkg, userId); throw publishingUnavailable();
    }
    @Override public boolean updateShortcuts(String pkg, ParceledListSlice list, int userId) {
        query(pkg, userId); throw publishingUnavailable();
    }
    @Override public void pushDynamicShortcut(String pkg, ShortcutInfo info, int userId) {
        query(pkg, userId); throw publishingUnavailable();
    }
    // No shortcuts have ever been published. Removal is idempotent and cannot
    // mutate another package's data; none of these paths creates a shortcut.
    @Override public void removeDynamicShortcuts(String pkg, List<String> ids, int userId) {
        query(pkg, userId); Objects.requireNonNull(ids, "shortcutIds");
    }
    @Override public void removeAllDynamicShortcuts(String pkg, int userId) { query(pkg, userId); }
    @Override public void removeLongLivedShortcuts(String pkg, List<String> ids, int userId) {
        query(pkg, userId); Objects.requireNonNull(ids, "shortcutIds");
    }
    @Override public void disableShortcuts(String pkg, List<String> ids, CharSequence message,
            int messageId, int userId) { query(pkg, userId); Objects.requireNonNull(ids, "shortcutIds"); }
    @Override public void enableShortcuts(String pkg, List<String> ids, int userId) {
        query(pkg, userId); Objects.requireNonNull(ids, "shortcutIds");
    }
    @Override public void reportShortcutUsed(String pkg, String id, int userId) {
        query(pkg, userId); Objects.requireNonNull(id, "shortcutId");
    }
    private static SecurityException administrationDenied() {
        return new SecurityException("Shortcut administration requires a trusted system caller");
    }
    @Override public void resetThrottling() { throw administrationDenied(); }
    @Override public void onApplicationActive(String pkg, int userId) { throw administrationDenied(); }
    @Override public byte[] getBackupPayload(int userId) { throw administrationDenied(); }
    @Override public void applyRestore(byte[] payload, int userId) { throw administrationDenied(); }
}
