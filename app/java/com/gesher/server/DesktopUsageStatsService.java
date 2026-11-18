/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.PendingIntent;
import android.app.usage.AppStandbyInfo;
import android.app.usage.BroadcastResponseStatsList;
import android.app.usage.ConfigurationStats;
import android.app.usage.EventStats;
import android.app.usage.IUsageStatsManager;
import android.app.usage.UsageEvents;
import android.app.usage.UsageEventsQuery;
import android.app.usage.UsageStats;
import android.app.usage.UsageStatsManager;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.SystemClock;
import android.os.UserHandle;

import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;
import java.util.Set;
import java.util.TreeSet;

/** Activity usage history for the managed desktop profile. Android app standby remains disabled. */
final class DesktopUsageStatsService extends IUsageStatsManager.Default {
    private final PackageManagerService mPackages;
    private final CallerIdentity mCallers;
    private final ActivityUsageHistory mHistory;
    private final boolean mManaged;

    DesktopUsageStatsService(PackageManagerService packages, CallerIdentity callers, Path file) {
        this(packages, callers, file, System::currentTimeMillis, SystemClock::elapsedRealtime);
    }

    DesktopUsageStatsService(PackageManagerService packages, CallerIdentity callers, Path file,
            java.util.function.LongSupplier clock) {
        this(packages, callers, file, clock, clock);
    }

    private DesktopUsageStatsService(PackageManagerService packages, CallerIdentity callers, Path file,
            java.util.function.LongSupplier wall, java.util.function.LongSupplier elapsed) {
        mPackages = packages;
        mCallers = callers;
        mManaged = file != null;
        mHistory = new ActivityUsageHistory(file, wall, elapsed,
                failure -> SystemServer.log("usage history I/O failed: "
                        + failure.getClass().getSimpleName()));
        if (mManaged) mPackages.onPackageRemoval(this::packageRemoved);
    }

    void transition(Object token, String pkg, String cls, ActivityUsageHistory.State state) {
        if (mManaged && (state == ActivityUsageHistory.State.TERMINATED
                || !mPackages.isRemovalPending(pkg))) {
            mHistory.transition(token, pkg, cls, state);
        }
    }

    private void packageRemoved(String pkg) {
        try { mHistory.forgetPackage(pkg); }
        catch (IOException failure) {
            throw new IllegalStateException("Cannot remove package usage history", failure);
        }
    }

    void flush() {
        try { mHistory.flush(); }
        catch (IOException failure) {
            SystemServer.log("usage history flush failed: " + failure.getClass().getSimpleName());
        }
    }

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

    private boolean usageAccess(String caller) {
        mCallers.requirePackage(Objects.requireNonNull(caller, "callingPackage"));
        // The permissive AppOps stand-in does not represent a user-granted Usage Access mode.
        return mCallers.isSystem() || mPackages.permissions().checkPermission(caller,
                Manifest.permission.PACKAGE_USAGE_STATS, null, 0) == PackageManager.PERMISSION_GRANTED;
    }

    private UsageEvents events(long begin, long end, Set<String> packages, int[] types) {
        ActivityUsageHistory.Snapshot snapshot = mHistory.snapshot();
        begin = Math.max(begin, snapshot.until() - ActivityUsageHistory.RETENTION);
        List<UsageEvents.Event> result = new ArrayList<>();
        TreeSet<String> strings = new TreeSet<>();
        for (ActivityUsageHistory.Event entry : snapshot.events()) {
            int type = eventType(entry.state());
            if (type == UsageEvents.Event.NONE || entry.time() < begin || entry.time() >= end
                    || !packages.isEmpty() && !packages.contains(entry.pkg())
                    || !matchesType(types, type)) {
                continue;
            }
            UsageEvents.Event event = new UsageEvents.Event();
            event.mPackage = entry.pkg();
            event.mClass = entry.cls();
            event.mInstanceId = entry.id();
            event.mTimeStamp = entry.time();
            event.mEventType = type;
            strings.add(event.mPackage);
            strings.add(event.mClass);
            result.add(event);
        }
        return new UsageEvents(result, strings.toArray(new String[0]));
    }

    private static boolean matchesType(int[] types, int type) {
        if (types.length == 0) return true;
        for (int wanted : types) if (wanted == type) return true;
        return false;
    }

    private static int eventType(ActivityUsageHistory.State state) {
        if (state == null) return UsageEvents.Event.NONE;
        return switch (state) {
            case RESUMED -> UsageEvents.Event.ACTIVITY_RESUMED;
            case PAUSED -> UsageEvents.Event.ACTIVITY_PAUSED;
            // AOSP exposes a destruction without a preceding stop as ACTIVITY_STOPPED.
            case STOPPED, TERMINATED -> UsageEvents.Event.ACTIVITY_STOPPED;
            default -> UsageEvents.Event.NONE;
        };
    }

    private static SecurityException denied() {
        return new SecurityException("Gesher does not implement Android usage-limit or standby administration");
    }

    @Override public ParceledListSlice<UsageStats> queryUsageStats(int interval, long begin, long end,
            String caller, int userId) {
        if (!usageAccess(caller)) return null;
        user(userId);
        if (interval < UsageStatsManager.INTERVAL_DAILY
                || interval > UsageStatsManager.INTERVAL_BEST || begin >= end) return null;
        List<UsageStats> stats = new ArrayList<>();
        for (ActivityUsageHistory.Total total :
                mHistory.aggregate(interval, begin, end)) {
            UsageStats result = new UsageStats();
            result.mPackageName = total.pkg;
            result.mBeginTimeStamp = total.begin;
            result.mEndTimeStamp = total.end;
            result.mTotalTimeInForeground = total.foreground;
            result.mTotalTimeVisible = total.visible;
            result.mLastTimeUsed = total.lastUsed;
            result.mLastTimeVisible = total.lastVisible;
            result.mLaunchCount = total.launches;
            result.mAppLaunchCount = total.appLaunches;
            result.mLastEvent = eventType(total.lastEvent);
            stats.add(result);
        }
        return new ParceledListSlice<>(stats);
    }

    @Override public ParceledListSlice<ConfigurationStats> queryConfigurationStats(int interval,
            long begin, long end, String caller) {
        return usageAccess(caller) ? ParceledListSlice.emptyList() : null;
    }
    @Override public ParceledListSlice<EventStats> queryEventStats(int interval, long begin,
            long end, String caller) {
        return usageAccess(caller) ? ParceledListSlice.emptyList() : null;
    }
    @Override public UsageEvents queryEvents(long begin, long end, String caller) {
        return queryEventsForUser(begin, end, 0, caller);
    }
    @Override public UsageEvents queryEventsForPackage(long begin, long end, String caller) {
        String actual = mCallers.requirePackage(Objects.requireNonNull(caller, "callingPackage"));
        return events(begin, end, Set.of(actual), new int[0]);
    }
    @Override public UsageEvents queryEventsForUser(long begin, long end, int userId, String caller) {
        if (!usageAccess(caller)) return null;
        user(userId);
        return events(begin, end, Set.of(), new int[0]);
    }
    @Override public UsageEvents queryEventsForPackageForUser(long begin, long end, int userId,
            String packageName, String caller) {
        if (!usageAccess(caller)) return null;
        user(userId);
        mCallers.requirePackage(packageName);
        return events(begin, end, Set.of(packageName), new int[0]);
    }
    @Override public UsageEvents queryEventsWithFilter(UsageEventsQuery query, String caller) {
        Objects.requireNonNull(query, "query");
        if (!usageAccess(caller)) return null;
        if (query.getUserId() != UserHandle.USER_NULL) user(query.getUserId());
        return events(query.getBeginTimeMillis(), query.getEndTimeMillis(),
                query.getPackageNames(), query.getEventTypes());
    }
    @Override public boolean isAppStandbyEnabled() { return false; }
    @Override public boolean isAppInactive(String packageName, int userId, String caller) {
        mCallers.requirePackage(caller);
        user(userId);
        return false;
    }
    @Override public int getAppStandbyBucket(String packageName, String caller, int userId) {
        mCallers.requirePackage(caller);
        if (!Objects.equals(packageName, caller) && !usageAccess(caller)) {
            throw new SecurityException("Caller does not have Usage Access");
        }
        installed(packageName, userId);
        return UsageStatsManager.STANDBY_BUCKET_ACTIVE;
    }
    @Override public int getAppMinStandbyBucket(String packageName, String caller, int userId) {
        return getAppStandbyBucket(packageName, caller, userId);
    }
    @Override public ParceledListSlice getAppStandbyBuckets(String caller, int userId) {
        if (!usageAccess(caller)) throw new SecurityException("Caller does not have Usage Access");
        user(userId);
        ArrayList<AppStandbyInfo> buckets = new ArrayList<>();
        for (ApkPackage pkg : mPackages.packages()) {
            buckets.add(new AppStandbyInfo(pkg.packageName, UsageStatsManager.STANDBY_BUCKET_ACTIVE));
        }
        return new ParceledListSlice<>(buckets);
    }
    @Override public int getUsageSource() { return UsageStatsManager.USAGE_SOURCE_TASK_ROOT_ACTIVITY; }
    @Override public long getLastTimeAnyComponentUsed(String packageName, String caller) {
        mCallers.requirePackage(caller);
        if (!mCallers.isSystem()) throw new SecurityException("Cross-user usages require system");
        return 0;
    }
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
    @Override public void reportUsageStart(IBinder token, String name, String caller) { }
    @Override public void reportPastUsageStart(IBinder token, String name, long ago, String caller) { }
    @Override public void reportUsageStop(IBinder token, String name, String caller) { }
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
