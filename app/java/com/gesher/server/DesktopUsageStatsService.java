/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.PendingIntent;
import android.app.usage.AppStandbyInfo;
import android.app.usage.BroadcastResponseStatsList;
import android.app.usage.IUsageStatsManager;
import android.app.usage.UsageEvents;
import android.app.usage.UsageEventsQuery;
import android.app.usage.UsageStatsManager;
import android.content.pm.ParceledListSlice;
import android.os.IBinder;
import android.os.PersistableBundle;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Objects;

/**
 * Readable empty usage history, with Android app standby disabled on Windows.
 * No activity telemetry or usage-limit timers are collected. In particular an
 * observer request cannot be accepted and then left waiting forever.
 */
class DesktopUsageStatsService extends IUsageStatsManager.Default {
    private final PackageManagerService mPackages;

    DesktopUsageStatsService(PackageManagerService packages) { mPackages = packages; }

    private static void user(int userId) {
        if (userId != 0) throw new IllegalArgumentException("Unknown user " + userId);
    }

    private void installed(String packageName, int userId) {
        user(userId);
        Objects.requireNonNull(packageName, "packageName");
        if (mPackages.getPackageInfo(packageName, 0, userId) == null) {
            throw new IllegalArgumentException("Unknown package " + packageName);
        }
    }

    private static UsageEvents events() {
        return new UsageEvents(Collections.emptyList(), new String[0]);
    }

    private static SecurityException denied() {
        return new SecurityException("Usage administration requires a trusted privileged caller; "
                + "Gesher does not collect Android usage-limit telemetry");
    }

    @Override public ParceledListSlice queryUsageStats(int interval, long begin, long end,
            String caller, int userId) { user(userId); return ParceledListSlice.emptyList(); }
    @Override public ParceledListSlice queryConfigurationStats(int interval, long begin, long end,
            String caller) { return ParceledListSlice.emptyList(); }
    @Override public ParceledListSlice queryEventStats(int interval, long begin, long end,
            String caller) { return ParceledListSlice.emptyList(); }
    @Override public UsageEvents queryEvents(long begin, long end, String caller) { return events(); }
    @Override public UsageEvents queryEventsForPackage(long begin, long end, String caller) {
        return events();
    }
    @Override public UsageEvents queryEventsForUser(long begin, long end, int userId, String caller) {
        user(userId); return events();
    }
    @Override public UsageEvents queryEventsForPackageForUser(long begin, long end, int userId,
            String packageName, String caller) { user(userId); return events(); }
    @Override public UsageEvents queryEventsWithFilter(UsageEventsQuery query, String caller) {
        Objects.requireNonNull(query, "query"); return events();
    }
    @Override public boolean isAppStandbyEnabled() { return false; }
    @Override public boolean isAppInactive(String packageName, int userId, String caller) {
        installed(packageName, userId); return false;
    }
    @Override public int getAppStandbyBucket(String packageName, String caller, int userId) {
        installed(packageName, userId); return UsageStatsManager.STANDBY_BUCKET_ACTIVE;
    }
    @Override public int getAppMinStandbyBucket(String packageName, String caller, int userId) {
        installed(packageName, userId); return UsageStatsManager.STANDBY_BUCKET_ACTIVE;
    }
    @Override public ParceledListSlice getAppStandbyBuckets(String caller, int userId) {
        user(userId);
        ArrayList<AppStandbyInfo> buckets = new ArrayList<>();
        for (ApkPackage pkg : mPackages.packages()) {
            buckets.add(new AppStandbyInfo(pkg.packageName, UsageStatsManager.STANDBY_BUCKET_ACTIVE));
        }
        return new ParceledListSlice<>(buckets);
    }
    @Override public int getUsageSource() { return UsageStatsManager.USAGE_SOURCE_TASK_ROOT_ACTIVITY; }
    @Override public long getLastTimeAnyComponentUsed(String packageName, String caller) { return 0; }
    @Override public BroadcastResponseStatsList queryBroadcastResponseStats(String packageName,
            long id, String caller, int userId) {
        user(userId); return new BroadcastResponseStatsList(Collections.emptyList());
    }
    @Override public boolean isPackageExemptedFromBroadcastResponseStats(String pkg, int userId) {
        installed(pkg, userId); return false;
    }
    @Override public String getAppStandbyConstant(String key) { return null; }

    @Override public void setAppInactive(String pkg, boolean inactive, int userId) { throw denied(); }
    @Override public void setAppStandbyBucket(String pkg, int bucket, int userId) { throw denied(); }
    @Override public void setAppStandbyBuckets(ParceledListSlice buckets, int userId) { throw denied(); }
    @Override public void setEstimatedLaunchTime(String pkg, long time, int userId) { throw denied(); }
    @Override public void setEstimatedLaunchTimes(ParceledListSlice times, int userId) { throw denied(); }
    @Override public void registerAppUsageObserver(int id, String[] packages, long limit,
            PendingIntent callback, String caller) { throw denied(); }
    @Override public void registerUsageSessionObserver(int id, String[] packages, long limit,
            long threshold, PendingIntent limitCallback, PendingIntent endCallback, String caller) {
        throw denied();
    }
    @Override public void registerAppUsageLimitObserver(int id, String[] packages, long limit,
            long used, PendingIntent callback, String caller) { throw denied(); }
    // Nothing can have registered; removal is idempotent and retains no callback/token.
    @Override public void unregisterAppUsageObserver(int id, String caller) { }
    @Override public void unregisterUsageSessionObserver(int id, String caller) { }
    @Override public void unregisterAppUsageLimitObserver(int id, String caller) { }
    @Override public void reportUsageStart(IBinder token, String name, String caller) { throw denied(); }
    @Override public void reportPastUsageStart(IBinder token, String name, long ago, String caller) {
        throw denied();
    }
    @Override public void reportUsageStop(IBinder token, String name, String caller) { throw denied(); }
    @Override public void reportChooserSelection(String pkg, int userId, String type,
            String[] annotations, String action) { }
    @Override public void reportUserInteraction(String pkg, int userId) { }
    @Override public void reportUserInteractionWithBundle(String pkg, int userId,
            PersistableBundle extras) { }
    @Override public void onCarrierPrivilegedAppsChanged() { throw denied(); }
    @Override public void forceUsageSourceSettingRead() { throw denied(); }
    @Override public void clearBroadcastResponseStats(String pkg, long id, String caller,
            int userId) { throw denied(); }
    @Override public void clearBroadcastEvents(String caller, int userId) { throw denied(); }
}
