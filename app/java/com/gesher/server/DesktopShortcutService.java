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

import com.gesher.app.shell.AppShell;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Windows has no Android shortcut host, share-target index, or pinned-item UI.
 * Queries describe that absence. Dynamic shortcut publishing operations store
 * shortcuts and publish Windows Start menu shortcuts into the Gesher Apps folder,
 * preserving compatibility with Android apps and AndroidX ShortcutManagerCompat.
 * Pin requests return false, for which Android specifies that no result callback is sent.
 */
class DesktopShortcutService extends IShortcutService.Default {
    private final PackageManagerService mPackages;
    private final Map<String, Map<String, ShortcutInfo>> mShortcutsByPackage = new ConcurrentHashMap<>();

    DesktopShortcutService(PackageManagerService packages) { mPackages = packages; }

    private void query(String packageName, int userId) {
        if (userId != 0) throw new IllegalArgumentException("Unknown user " + userId);
        Objects.requireNonNull(packageName, "packageName");
        if (mPackages.getPackageInfo(packageName, 0, userId) == null) {
            throw new IllegalArgumentException("Unknown package " + packageName);
        }
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
        query(pkg, userId);
        Map<String, ShortcutInfo> map = mShortcutsByPackage.get(pkg);
        if (map == null || map.isEmpty()) return ParceledListSlice.emptyList();
        return new ParceledListSlice<>(new ArrayList<>(map.values()));
    }
    @Override public ParceledListSlice getShareTargets(String pkg, IntentFilter filter, int userId) {
        query(pkg, userId); Objects.requireNonNull(filter, "filter");
        return ParceledListSlice.emptyList();
    }
    @Override public boolean hasShareTargets(String caller, String packageName, int userId) {
        query(caller, userId); return false;
    }
    @Override public int getMaxShortcutCountPerActivity(String pkg, int userId) {
        query(pkg, userId); return 15;
    }
    @Override public int getRemainingCallCount(String pkg, int userId) {
        query(pkg, userId); return 10;
    }
    @Override public long getRateLimitResetTime(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public int getIconMaxDimensions(String pkg, int userId) {
        query(pkg, userId); return 0;
    }
    @Override public boolean setDynamicShortcuts(String pkg, ParceledListSlice list, int userId) {
        query(pkg, userId);
        List<?> raw = list != null ? list.getList() : null;
        List<ShortcutInfo> shortcuts = new ArrayList<>();
        if (raw != null) {
            for (Object o : raw) {
                if (o instanceof ShortcutInfo) shortcuts.add((ShortcutInfo) o);
            }
        }
        Map<String, ShortcutInfo> map = new LinkedHashMap<>();
        for (ShortcutInfo si : shortcuts) {
            if (si.getId() != null) map.put(si.getId(), si);
        }
        mShortcutsByPackage.put(pkg, map);
        try {
            AppShell.syncDynamicShortcuts(pkg, shortcuts, false);
        } catch (Throwable ignored) { }
        return true;
    }
    @Override public boolean addDynamicShortcuts(String pkg, ParceledListSlice list, int userId) {
        query(pkg, userId);
        List<?> raw = list != null ? list.getList() : null;
        List<ShortcutInfo> shortcuts = new ArrayList<>();
        if (raw != null) {
            for (Object o : raw) {
                if (o instanceof ShortcutInfo) shortcuts.add((ShortcutInfo) o);
            }
        }
        Map<String, ShortcutInfo> map = mShortcutsByPackage.computeIfAbsent(pkg, k -> new LinkedHashMap<>());
        synchronized (map) {
            for (ShortcutInfo si : shortcuts) {
                if (si.getId() != null) map.put(si.getId(), si);
            }
        }
        try {
            AppShell.syncDynamicShortcuts(pkg, shortcuts, true);
        } catch (Throwable ignored) { }
        return true;
    }
    @Override public boolean updateShortcuts(String pkg, ParceledListSlice list, int userId) {
        return addDynamicShortcuts(pkg, list, userId);
    }
    @Override public void pushDynamicShortcut(String pkg, ShortcutInfo info, int userId) {
        query(pkg, userId);
        if (info == null || info.getId() == null) return;
        Map<String, ShortcutInfo> map = mShortcutsByPackage.computeIfAbsent(pkg, k -> new LinkedHashMap<>());
        synchronized (map) {
            map.put(info.getId(), info);
        }
        try {
            AppShell.syncDynamicShortcuts(pkg, Collections.singletonList(info), true);
        } catch (Throwable ignored) { }
    }
    // Removal updates both in-memory state and Windows shell shortcuts.
    @Override public void removeDynamicShortcuts(String pkg, List<String> ids, int userId) {
        query(pkg, userId); Objects.requireNonNull(ids, "shortcutIds");
        Map<String, ShortcutInfo> map = mShortcutsByPackage.get(pkg);
        if (map != null) {
            synchronized (map) {
                for (String id : ids) map.remove(id);
            }
        }
        try {
            AppShell.removeDynamicShortcuts(pkg, ids);
        } catch (Throwable ignored) { }
    }
    @Override public void removeAllDynamicShortcuts(String pkg, int userId) {
        query(pkg, userId);
        mShortcutsByPackage.remove(pkg);
        try {
            AppShell.removeAllDynamicShortcuts(pkg);
        } catch (Throwable ignored) { }
    }
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
