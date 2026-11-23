#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Contract tests and deterministic ABBA benchmark for F31 AppUsageObserver.

Compiles the real production ActivityUsageHistory, UsageLimitObservers,
DesktopUsageStatsService and CallerIdentity on a host JDK and verifies:
  - OBSERVE_APP_USAGE permission and CallerIdentity.requirePackage enforcement
  - Empty/null package array, null callback, and timeLimitMs >= 60_000L checks
  - Per-UID 1000-observer limit and same-observerId replacement semantics
  - Event-driven foreground accumulation across multiple packages and overlaps
  - Single scheduled expiry task, PendingIntent extras, and one-shot removal
  - Deterministic ABBA (cand1, base1, base2, cand2) proving 0 idle wakeups
    and negligible transition(...) overhead relative to a baseline git ref
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS: dict[str, str] = {
    "android/Manifest.java": """package android;
public final class Manifest {
    public static final class permission {
        public static final String PACKAGE_USAGE_STATS = "android.permission.PACKAGE_USAGE_STATS";
        public static final String OBSERVE_APP_USAGE = "android.permission.OBSERVE_APP_USAGE";
    }
}
""",
    "android/os/IBinder.java": """package android.os;
public interface IBinder {}
""",
    "android/os/IInterface.java": """package android.os;
public interface IInterface {
    default IBinder asBinder() { return null; }
}
""",
    "android/os/Process.java": """package android.os;
public final class Process {
    public static final int SYSTEM_UID = 1000;
    public static final int SHELL_UID = 2000;
    private static final int MY_PID = 4242;
    public static int myPid() { return MY_PID; }
    public static int myUid() { return SYSTEM_UID; }
}
""",
    "android/os/Binder.java": """package android.os;
public class Binder implements IBinder {
    private static final ThreadLocal<int[]> CALLER =
            ThreadLocal.withInitial(() -> new int[] {Process.myPid(), Process.SYSTEM_UID});
    public static void setCallerForTest(int pid, int uid) {
        CALLER.set(new int[] {pid, uid});
    }
    public static void resetCallerForTest() {
        CALLER.set(new int[] {Process.myPid(), Process.SYSTEM_UID});
    }
    public static int getCallingPid() { return CALLER.get()[0]; }
    public static int getCallingUid() { return CALLER.get()[1]; }
    public static long clearCallingIdentity() {
        int[] cur = CALLER.get();
        long token = (((long) cur[0]) << 32) | (cur[1] & 0xffffffffL);
        resetCallerForTest();
        return token;
    }
    public static void restoreCallingIdentity(long token) {
        CALLER.set(new int[] {(int) (token >> 32), (int) token});
    }
}
""",
    "android/os/SystemClock.java": """package android.os;
public final class SystemClock {
    public static long elapsedRealtime() {
        return System.nanoTime() / 1_000_000L;
    }
}
""",
    "android/os/UserHandle.java": """package android.os;
public final class UserHandle {
    public static final int USER_NULL = -10000;
}
""",
    "android/os/Bundle.java": """package android.os;
public final class Bundle {}
""",
    "android/os/PersistableBundle.java": """package android.os;
public final class PersistableBundle {}
""",
    "android/content/Context.java": """package android.content;
public abstract class Context {}
""",
    "android/content/Intent.java": """package android.content;
import java.util.LinkedHashMap;
import java.util.Map;
public class Intent {
    private final Map<String, Object> mExtras = new LinkedHashMap<>();
    public Intent() {}
    public Intent(Intent other) {
        if (other != null) mExtras.putAll(other.mExtras);
    }
    public Intent putExtra(String name, int value) {
        mExtras.put(name, value);
        return this;
    }
    public Intent putExtra(String name, long value) {
        mExtras.put(name, value);
        return this;
    }
    public Intent putExtra(String name, String value) {
        mExtras.put(name, value);
        return this;
    }
    public int getIntExtra(String name, int defaultValue) {
        Object v = mExtras.get(name);
        return v instanceof Integer ? (Integer) v : defaultValue;
    }
    public long getLongExtra(String name, long defaultValue) {
        Object v = mExtras.get(name);
        return v instanceof Long ? (Long) v : defaultValue;
    }
    public String getStringExtra(String name) {
        Object v = mExtras.get(name);
        return v instanceof String ? (String) v : null;
    }
    public void fillInFrom(Intent other) {
        if (other != null) mExtras.putAll(other.mExtras);
    }
}
""",
    "android/content/IIntentReceiver.java": """package android.content;
public interface IIntentReceiver {}
""",
    "android/content/IIntentSender.java": """package android.content;
import android.os.Bundle;
import android.os.IBinder;
public interface IIntentSender {
    void send(int code, Intent intent, String resolvedType, IBinder allowlistToken,
            IIntentReceiver finishedReceiver, String requiredPermission, Bundle options);
}
""",
    "android/app/PendingIntent.java": """package android.app;
import android.content.Context;
import android.content.IIntentSender;
import android.content.Intent;
public final class PendingIntent {
    public static class CanceledException extends Exception {}
    private final IIntentSender mTarget;
    private final Intent mBaseIntent;
    private volatile boolean mCanceled;
    public PendingIntent(IIntentSender target) {
        this(target, new Intent());
    }
    public PendingIntent(IIntentSender target, Intent baseIntent) {
        mTarget = target;
        mBaseIntent = baseIntent == null ? new Intent() : new Intent(baseIntent);
    }
    public void cancel() { mCanceled = true; }
    public IIntentSender getTarget() { return mTarget; }
    public void send(Context context, int code, Intent intent) throws CanceledException {
        if (mCanceled || mTarget == null) throw new CanceledException();
        Intent merged = new Intent(mBaseIntent);
        merged.fillInFrom(intent);
        mTarget.send(code, merged, null, null, null, null, null);
    }
}
""",
    "android/app/AppOpsManager.java": """package android.app;
public final class AppOpsManager {
    public static final int MODE_ALLOWED = 0;
    public static final int MODE_IGNORED = 1;
    public static final int MODE_ERRORED = 2;
    public static final int MODE_DEFAULT = 3;
    public static final int OP_GET_USAGE_STATS = 43;
    public static final class OpResult {
        private final int mMode;
        public OpResult(int mode) { mMode = mode; }
        public int getOpMode() { return mMode; }
    }
}
""",
    "android/content/pm/PackageInfo.java": """package android.content.pm;
public class PackageInfo {
    public String packageName;
}
""",
    "android/content/pm/PackageManager.java": """package android.content.pm;
public abstract class PackageManager {
    public static final int PERMISSION_GRANTED = 0;
    public static final int PERMISSION_DENIED = -1;
}
""",
    "android/content/pm/ParceledListSlice.java": """package android.content.pm;
import java.util.Collections;
import java.util.List;
public final class ParceledListSlice<T> {
    private final List<T> mList;
    public ParceledListSlice(List<T> list) { mList = list; }
    public List<T> getList() { return mList; }
    public static <T> ParceledListSlice<T> emptyList() {
        return new ParceledListSlice<>(Collections.emptyList());
    }
}
""",
    "android/app/usage/UsageStatsManager.java": """package android.app.usage;
public final class UsageStatsManager {
    public static final int INTERVAL_DAILY = 0;
    public static final int INTERVAL_WEEKLY = 1;
    public static final int INTERVAL_MONTHLY = 2;
    public static final int INTERVAL_YEARLY = 3;
    public static final int INTERVAL_BEST = 4;
    public static final int STANDBY_BUCKET_ACTIVE = 10;
    public static final int USAGE_SOURCE_TASK_ROOT_ACTIVITY = 1;
    public static final String EXTRA_OBSERVER_ID = "android.app.usage.extra.OBSERVER_ID";
    public static final String EXTRA_TIME_LIMIT = "android.app.usage.extra.TIME_LIMIT";
    public static final String EXTRA_TIME_USED = "android.app.usage.extra.TIME_USED";
}
""",
    "android/app/usage/UsageEvents.java": """package android.app.usage;
import java.util.List;
public final class UsageEvents {
    public static final class Event {
        public static final int NONE = 0;
        public static final int ACTIVITY_RESUMED = 1;
        public static final int ACTIVITY_PAUSED = 2;
        public static final int ACTIVITY_STOPPED = 23;
        public String mPackage;
        public String mClass;
        public int mInstanceId;
        public long mTimeStamp;
        public int mEventType;
    }
    private final List<Event> mEvents;
    private int mIndex;
    public UsageEvents(List<Event> events, String[] stringPool) { mEvents = events; }
    public boolean hasNextEvent() { return mIndex < mEvents.size(); }
    public boolean getNextEvent(Event out) {
        if (!hasNextEvent()) return false;
        Event e = mEvents.get(mIndex++);
        out.mPackage = e.mPackage;
        out.mClass = e.mClass;
        out.mInstanceId = e.mInstanceId;
        out.mTimeStamp = e.mTimeStamp;
        out.mEventType = e.mEventType;
        return true;
    }
}
""",
    "android/app/usage/UsageEventsQuery.java": """package android.app.usage;
import java.util.Set;
public final class UsageEventsQuery {
    private final long mBegin, mEnd;
    private final int mUserId;
    private final Set<String> mPackages;
    private final int[] mTypes;
    public UsageEventsQuery(long begin, long end, int userId, Set<String> packages, int[] types) {
        mBegin = begin; mEnd = end; mUserId = userId; mPackages = packages; mTypes = types;
    }
    public long getBeginTimeMillis() { return mBegin; }
    public long getEndTimeMillis() { return mEnd; }
    public int getUserId() { return mUserId; }
    public Set<String> getPackageNames() { return mPackages; }
    public int[] getEventTypes() { return mTypes; }
}
""",
    "android/app/usage/UsageStats.java": """package android.app.usage;
public final class UsageStats {
    public String mPackageName;
    public long mBeginTimeStamp;
    public long mEndTimeStamp;
    public long mTotalTimeInForeground;
    public long mTotalTimeVisible;
    public long mLastTimeUsed;
    public long mLastTimeVisible;
    public int mLaunchCount;
    public int mAppLaunchCount;
    public int mLastEvent;
}
""",
    "android/app/usage/AppStandbyInfo.java": """package android.app.usage;
public final class AppStandbyInfo {
    public final String mPackageName;
    public final int mStandbyBucket;
    public AppStandbyInfo(String pkg, int bucket) { mPackageName = pkg; mStandbyBucket = bucket; }
}
""",
    "android/app/usage/BroadcastResponseStatsList.java": """package android.app.usage;
import java.util.List;
public final class BroadcastResponseStatsList {
    private final List<?> mList;
    public BroadcastResponseStatsList(List<?> list) { mList = list; }
    public List<?> getList() { return mList; }
}
""",
    "android/app/usage/ConfigurationStats.java": """package android.app.usage;
public final class ConfigurationStats {}
""",
    "android/app/usage/EventStats.java": """package android.app.usage;
public final class EventStats {}
""",
    "android/app/usage/IUsageStatsManager.java": """package android.app.usage;
import android.app.PendingIntent;
import android.content.pm.ParceledListSlice;
import android.os.IBinder;
import android.os.IInterface;
import android.os.PersistableBundle;
public interface IUsageStatsManager extends IInterface {
    ParceledListSlice<UsageStats> queryUsageStats(int interval, long begin, long end, String caller, int userId);
    ParceledListSlice<ConfigurationStats> queryConfigurationStats(int interval, long begin, long end, String caller);
    ParceledListSlice<EventStats> queryEventStats(int interval, long begin, long end, String caller);
    UsageEvents queryEvents(long begin, long end, String caller);
    UsageEvents queryEventsForPackage(long begin, long end, String caller);
    UsageEvents queryEventsForUser(long begin, long end, int userId, String caller);
    UsageEvents queryEventsForPackageForUser(long begin, long end, int userId, String packageName, String caller);
    UsageEvents queryEventsWithFilter(UsageEventsQuery query, String caller);
    boolean isAppStandbyEnabled();
    boolean isAppInactive(String packageName, int userId, String caller);
    int getAppStandbyBucket(String packageName, String caller, int userId);
    int getAppMinStandbyBucket(String packageName, String caller, int userId);
    ParceledListSlice getAppStandbyBuckets(String caller, int userId);
    int getUsageSource();
    long getLastTimeAnyComponentUsed(String packageName, String caller);
    BroadcastResponseStatsList queryBroadcastResponseStats(String packageName, long id, String caller, int userId);
    boolean isPackageExemptedFromBroadcastResponseStats(String pkg, int userId);
    String getAppStandbyConstant(String key);
    void setAppInactive(String pkg, boolean inactive, int userId);
    void setAppStandbyBucket(String pkg, int bucket, int userId);
    void setAppStandbyBuckets(ParceledListSlice buckets, int userId);
    void setEstimatedLaunchTime(String pkg, long time, int userId);
    void setEstimatedLaunchTimes(ParceledListSlice times, int userId);
    void registerAppUsageObserver(int id, String[] packages, long limit, PendingIntent callback, String caller);
    void registerUsageSessionObserver(int id, String[] packages, long limit, long threshold,
            PendingIntent limitCallback, PendingIntent endCallback, String caller);
    void registerAppUsageLimitObserver(int id, String[] packages, long limit, long used,
            PendingIntent callback, String caller);
    void unregisterAppUsageObserver(int id, String caller);
    void unregisterUsageSessionObserver(int id, String caller);
    void unregisterAppUsageLimitObserver(int id, String caller);
    void reportUsageStart(IBinder token, String name, String caller);
    void reportPastUsageStart(IBinder token, String name, long ago, String caller);
    void reportUsageStop(IBinder token, String name, String caller);
    void reportChooserSelection(String pkg, int userId, String type, String[] annotations, String action);
    void reportUserInteraction(String pkg, int userId);
    void reportUserInteractionWithBundle(String pkg, int userId, PersistableBundle extras);
    void onCarrierPrivilegedAppsChanged();
    void forceUsageSourceSettingRead();
    void clearBroadcastResponseStats(String pkg, long id, String caller, int userId);
    void clearBroadcastEvents(String caller, int userId);

    abstract class Default implements IUsageStatsManager {
        @Override public IBinder asBinder() { return null; }
    }
}
""",
    "com/gesher/server/SystemServer.java": """package com.gesher.server;
final class SystemServer {
    static void log(String message) {}
}
""",
    "com/gesher/server/ApkPackage.java": """package com.gesher.server;
public final class ApkPackage {
    public final String packageName;
    private final int mUid;
    ApkPackage(String packageName, int uid) {
        this.packageName = packageName;
        this.mUid = uid;
    }
    public int uid() { return mUid; }
}
""",
    "com/gesher/server/ActivityTaskManagerService.java": """package com.gesher.server;
import java.util.HashMap;
import java.util.Map;
public final class ActivityTaskManagerService {
    private final Map<Integer, ApkPackage> mPidPackages = new HashMap<>();
    void bindPid(int pid, ApkPackage pkg) {
        if (pkg == null) mPidPackages.remove(pid);
        else mPidPackages.put(pid, pkg);
    }
    String authenticatedPackageForPid(int pid) {
        ApkPackage pkg = mPidPackages.get(pid);
        return pkg == null ? null : pkg.packageName;
    }
    ApkPackage authenticatedPackageCodeForPid(int pid, int uid) {
        ApkPackage pkg = mPidPackages.get(pid);
        return pkg != null && pkg.uid() == uid ? pkg : null;
    }
}
""",
    "com/gesher/server/PackageManagerService.java": """package com.gesher.server;
import android.app.AppOpsManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
public final class PackageManagerService {
    final class AppOps {
        int mode = AppOpsManager.MODE_DEFAULT;
        Runnable onCheck;
        AppOpsManager.OpResult noteOperation(int op, int uid, String pkg, String attr,
                boolean fg, String msg, boolean collect) {
            if (onCheck != null) onCheck.run();
            return new AppOpsManager.OpResult(mode);
        }
    }
    final class Permissions {
        private final Set<String> mGrants = new HashSet<>();
        Runnable onCheck;
        void grant(int uid, String permission) { mGrants.add(uid + ":" + permission); }
        void revoke(int uid, String permission) { mGrants.remove(uid + ":" + permission); }
        int checkProcessPermission(String permission, int pid, int uid) {
            if (onCheck != null) onCheck.run();
            return mGrants.contains(uid + ":" + permission)
                    ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }
    }
    private final Map<String, ApkPackage> mPackages = new LinkedHashMap<>();
    private final Set<String> mPendingRemoval = new HashSet<>();
    private final List<Consumer<String>> mRemovalListeners = new ArrayList<>();
    private final AppOps mAppOps = new AppOps();
    private final Permissions mPermissions = new Permissions();

    void install(String name, int uid) {
        mPackages.put(name, new ApkPackage(name, uid));
    }
    void setRemovalPending(String name, boolean pending) {
        if (pending) mPendingRemoval.add(name);
        else mPendingRemoval.remove(name);
    }
    void uninstall(String name) {
        mPendingRemoval.add(name);
        mPackages.remove(name);
        mPendingRemoval.remove(name);
        for (Consumer<String> listener : mRemovalListeners) listener.accept(name);
    }
    ApkPackage get(String name) { return mPackages.get(name); }
    Collection<ApkPackage> packages() { return mPackages.values(); }
    boolean isRemovalPending(String name) { return mPendingRemoval.contains(name); }
    void onPackageRemoval(Consumer<String> listener) { mRemovalListeners.add(listener); }
    PackageInfo getPackageInfo(String name, long flags, int userId) {
        if (!mPackages.containsKey(name)) return null;
        PackageInfo info = new PackageInfo();
        info.packageName = name;
        return info;
    }
    String[] getPackagesForUid(int uid) {
        List<String> list = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            if (p.uid() == uid && !mPendingRemoval.contains(p.packageName)) list.add(p.packageName);
        }
        return list.isEmpty() ? null : list.toArray(new String[0]);
    }
    AppOps appOps() { return mAppOps; }
    Permissions permissions() { return mPermissions; }
}
""",
    "com/gesher/server/ActivityManagerService.java": """package com.gesher.server;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
public final class ActivityManagerService {
    int sentCount;
    public int sendIntentSender(Object caller, IIntentSender target, IBinder whitelistToken,
            int code, Intent intent, String resolvedType, IIntentReceiver finishedReceiver,
            String requiredPermission, Bundle options) {
        sentCount++;
        if (target != null) {
            target.send(code, intent, resolvedType, whitelistToken, finishedReceiver,
                    requiredPermission, options);
        }
        return 0;
    }
}
""",
}

CONTRACT_HARNESS = r"""package com.gesher.server;

import static com.gesher.server.ActivityUsageHistory.State.PAUSED;
import static com.gesher.server.ActivityUsageHistory.State.RESUMED;
import static com.gesher.server.ActivityUsageHistory.State.STOPPED;
import static com.gesher.server.ActivityUsageHistory.State.TERMINATED;

import android.Manifest;
import android.app.PendingIntent;
import android.app.usage.UsageStatsManager;
import android.content.Intent;
import android.os.Binder;
import android.os.Process;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;

public final class UsageLimitObserversContractTest {
    private static int checks;

    private static void check(boolean cond, String message) {
        if (!cond) throw new AssertionError(message);
        checks++;
    }

    private record Delivery(int observerId, long timeLimit, long timeUsed, String tag) {}

    private static PendingIntent recordingCallback(List<Delivery> out, String tag) {
        Intent base = new Intent().putExtra("tag", tag);
        return new PendingIntent((code, intent, resolvedType, token, receiver, perm, opts) -> {
            Intent merged = new Intent(base);
            merged.fillInFrom(intent);
            synchronized (out) {
                out.add(new Delivery(
                        merged.getIntExtra(UsageStatsManager.EXTRA_OBSERVER_ID, -999),
                        merged.getLongExtra(UsageStatsManager.EXTRA_TIME_LIMIT, -999L),
                        merged.getLongExtra(UsageStatsManager.EXTRA_TIME_USED, -999L),
                        merged.getStringExtra("tag")));
                out.notifyAll();
            }
        }, base);
    }

    private static final class ManualScheduler implements UsageLimitObservers.ExpiryScheduler {
        record Task(long fireAtMs, long delayMs, Runnable runnable, boolean[] canceled) {}
        private final LongSupplier mNow;
        final List<Task> live = new ArrayList<>();
        long wakeups;

        ManualScheduler(LongSupplier now) {
            mNow = now;
        }

        @Override
        public synchronized UsageLimitObservers.Cancelable schedule(long delayMs, Runnable task) {
            boolean[] canceled = new boolean[] {false};
            Task entry = new Task(mNow.getAsLong() + delayMs, delayMs, task, canceled);
            live.add(entry);
            return () -> {
                synchronized (ManualScheduler.this) {
                    canceled[0] = true;
                    live.remove(entry);
                }
            };
        }

        @Override
        public synchronized long wakeupCount() { return wakeups; }

        @Override
        public synchronized int scheduledCount() { return live.size(); }

        long soleRemainingMs() {
            synchronized (this) {
                check(live.size() == 1, "expected exactly 1 scheduled expiry task, found " + live.size());
                return live.get(0).fireAtMs() - mNow.getAsLong();
            }
        }

        void fireEarliest() {
            Task target;
            synchronized (this) {
                check(!live.isEmpty(), "no scheduled task to fire");
                target = live.remove(0);
                if (target.canceled[0]) return;
                wakeups++;
            }
            target.runnable.run();
        }
    }

    private static void permissionAndValidationContracts() throws Exception {
        PackageManagerService pm = new PackageManagerService();
        ActivityTaskManagerService atm = new ActivityTaskManagerService();
        CallerIdentity callers = new CallerIdentity(pm, atm);
        pm.install("com.priv.observer", 10010);
        pm.install("com.regular.app", 10020);
        pm.install("com.other.app", 10030);
        atm.bindPid(5001, pm.get("com.priv.observer"));
        atm.bindPid(5002, pm.get("com.regular.app"));
        atm.bindPid(5003, pm.get("com.other.app"));
        pm.permissions().grant(10010, Manifest.permission.OBSERVE_APP_USAGE);
        pm.permissions().grant(10020, Manifest.permission.PACKAGE_USAGE_STATS);

        AtomicLong clock = new AtomicLong(1_000L);
        ManualScheduler scheduler = new ManualScheduler(clock::get);
        DesktopUsageStatsService svc = new DesktopUsageStatsService(
                pm, callers, null, clock::get, clock::get, scheduler);
        List<Delivery> deliveries = new ArrayList<>();
        PendingIntent cb = recordingCallback(deliveries, "v");

        try {
            // 1. Null caller
            Binder.resetCallerForTest();
            try {
                svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, 60_000L, cb, null);
                throw new AssertionError("accepted null caller");
            } catch (NullPointerException expected) { checks++; }

            // 2. Unprivileged app (even with PACKAGE_USAGE_STATS) denied on register & unregister
            Binder.setCallerForTest(5002, 10020);
            try {
                svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, 60_000L, cb,
                        "com.regular.app");
                throw new AssertionError("accepted caller without OBSERVE_APP_USAGE");
            } catch (SecurityException expected) { checks++; }
            try {
                svc.unregisterAppUsageObserver(1, "com.regular.app");
                throw new AssertionError("accepted unregister without OBSERVE_APP_USAGE");
            } catch (SecurityException expected) { checks++; }

            // 3. Privileged app spoofing another UID's package denied via requirePackage
            Binder.setCallerForTest(5001, 10010);
            try {
                svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, 60_000L, cb,
                        "com.other.app");
                throw new AssertionError("accepted spoofed caller package");
            } catch (SecurityException expected) { checks++; }

            // 4. Removal-pending race during permission check fails closed
            pm.permissions().onCheck = () -> pm.setRemovalPending("com.priv.observer", true);
            try {
                svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, 60_000L, cb,
                        "com.priv.observer");
                throw new AssertionError("accepted removal-pending observer owner");
            } catch (SecurityException expected) { checks++; }
            pm.permissions().onCheck = null;
            pm.setRemovalPending("com.priv.observer", false);

            // 5. Validation: null/empty packages, null/empty element, null callback, limit < 60_000L
            String[][] badPackages = {null, new String[0], new String[] {(String) null},
                    new String[] {""}, new String[] {"com.regular.app", ""}};
            for (String[] bad : badPackages) {
                try {
                    svc.registerAppUsageObserver(1, bad, 60_000L, cb, "com.priv.observer");
                    throw new AssertionError("accepted invalid package array");
                } catch (IllegalArgumentException expected) { checks++; }
            }
            try {
                svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, 60_000L, null,
                        "com.priv.observer");
                throw new AssertionError("accepted null PendingIntent");
            } catch (NullPointerException expected) { checks++; }

            for (long badLimit : new long[] {Long.MIN_VALUE, -1L, 0L, 1L, 59_999L}) {
                try {
                    svc.registerAppUsageObserver(1, new String[] {"com.regular.app"}, badLimit, cb,
                            "com.priv.observer");
                    throw new AssertionError("accepted timeLimitMs=" + badLimit);
                } catch (IllegalArgumentException expected) { checks++; }
            }
        } finally {
            Binder.resetCallerForTest();
            svc.observers().close();
        }
    }

    private static void perUidCapAndReplacementContracts() throws Exception {
        PackageManagerService pm = new PackageManagerService();
        ActivityTaskManagerService atm = new ActivityTaskManagerService();
        CallerIdentity callers = new CallerIdentity(pm, atm);
        pm.install("com.uid.one", 10101);
        pm.install("com.uid.two", 10102);
        atm.bindPid(6001, pm.get("com.uid.one"));
        atm.bindPid(6002, pm.get("com.uid.two"));
        pm.permissions().grant(10101, Manifest.permission.OBSERVE_APP_USAGE);
        pm.permissions().grant(10102, Manifest.permission.OBSERVE_APP_USAGE);

        AtomicLong clock = new AtomicLong(5_000L);
        ManualScheduler scheduler = new ManualScheduler(clock::get);
        DesktopUsageStatsService svc = new DesktopUsageStatsService(
                pm, callers, null, clock::get, clock::get, scheduler);
        List<Delivery> deliveries = new ArrayList<>();
        try {
            Binder.setCallerForTest(6001, 10101);
            for (int i = 0; i < 1_000; i++) {
                svc.registerAppUsageObserver(i, new String[] {"pkg." + (i % 8)}, 60_000L,
                        recordingCallback(deliveries, "old-" + i), "com.uid.one");
            }
            check(svc.observers().observerCountForUid(10101) == 1_000, "expected 1000 observers");
            check(scheduler.scheduledCount() == 0, "idle observers scheduled background work");

            // 1001st distinct ID for UID 10101 must fail with IllegalStateException
            try {
                svc.registerAppUsageObserver(1_000, new String[] {"pkg.extra"}, 60_000L,
                        recordingCallback(deliveries, "overflow"), "com.uid.one");
                throw new AssertionError("exceeded 1000 observers for UID");
            } catch (IllegalStateException expected) { checks++; }

            // Replacing an existing observerId at cap (1000) must succeed and replace callback/usage
            Object tok = new Object();
            svc.transition(tok, "pkg.5", "Main", RESUMED);
            clock.addAndGet(30_000L);
            svc.transition(tok, "pkg.5", "Main", PAUSED);

            svc.registerAppUsageObserver(5, new String[] {"pkg.5"}, 60_000L,
                    recordingCallback(deliveries, "replaced-5"), "com.uid.one");
            check(svc.observers().observerCountForUid(10101) == 1_000,
                    "replacing observer changed count");

            // Another UID has an independent 1000-observer quota and cannot unregister UID 10101's ID 5
            Binder.setCallerForTest(6002, 10102);
            svc.registerAppUsageObserver(5, new String[] {"pkg.other"}, 60_000L,
                    recordingCallback(deliveries, "uid2-5"), "com.uid.two");
            svc.unregisterAppUsageObserver(5, "com.uid.two");
            check(svc.observers().observerCountForUid(10102) == 0, "uid2 observer not removed");
            check(svc.observers().observerCountForUid(10101) == 1_000,
                    "uid2 unregister removed uid1 observer");

            // Verify replaced observer 5 reset its accumulated time (needs full 60,000ms, not 30,000ms)
            svc.transition(tok, "pkg.5", "Main", RESUMED);
            clock.addAndGet(40_000L);
            svc.transition(tok, "pkg.5", "Main", PAUSED);
            check(deliveries.stream().noneMatch(d -> d.observerId() == 5),
                    "replaced observer kept stale accumulated time");
            svc.transition(tok, "pkg.5", "Main", RESUMED);
            clock.addAndGet(20_000L);
            svc.transition(tok, "pkg.5", "Main", STOPPED);
            Delivery hit5 = deliveries.stream().filter(d -> d.observerId() == 5)
                    .findFirst().orElse(null);
            check(hit5 != null && "replaced-5".equals(hit5.tag()) && hit5.timeUsed() == 60_000L,
                    "replaced observer did not fire with new callback and fresh 60s total");
        } finally {
            Binder.resetCallerForTest();
            svc.observers().close();
        }
    }

    private static void foregroundAccumulationAndSingleTimerContracts() throws Exception {
        Path tempDir = Files.createTempDirectory("gesher-usage-obs");
        Path store = tempDir.resolve("activity-usage.bin");
        PackageManagerService pm = new PackageManagerService();
        ActivityTaskManagerService atm = new ActivityTaskManagerService();
        CallerIdentity callers = new CallerIdentity(pm, atm);
        ActivityManagerService am = new ActivityManagerService();
        pm.install("com.observer.owner", 10200);
        pm.install("pkg.alpha", 10201);
        pm.install("pkg.beta", 10202);
        atm.bindPid(7001, pm.get("com.observer.owner"));
        pm.permissions().grant(10200, Manifest.permission.OBSERVE_APP_USAGE);

        AtomicLong wall = new AtomicLong(1_700_000_000_000L);
        AtomicLong elapsed = new AtomicLong(10_000L);
        ManualScheduler scheduler = new ManualScheduler(elapsed::get);
        DesktopUsageStatsService svc = new DesktopUsageStatsService(
                pm, callers, store, wall::get, elapsed::get, scheduler);
        svc.setActivityManager(am);
        List<Delivery> deliveries = new ArrayList<>();

        try {
            Binder.setCallerForTest(7001, 10200);
            svc.registerAppUsageObserver(42, new String[] {"pkg.alpha", "pkg.beta", "pkg.alpha"},
                    60_000L, recordingCallback(deliveries, "group-42"), "com.observer.owner");
            svc.registerAppUsageObserver(99, new String[] {"pkg.alpha"},
                    90_000L, recordingCallback(deliveries, "alpha-99"), "com.observer.owner");
            check(scheduler.scheduledCount() == 0 && scheduler.wakeupCount() == 0,
                    "background registration scheduled a timer");

            // PAUSED-only visibility must NOT count as foreground or arm a timer
            Object pausedOnly = new Object();
            svc.transition(pausedOnly, "pkg.alpha", "DialogAct", PAUSED);
            wall.addAndGet(50_000L); elapsed.addAndGet(50_000L);
            svc.transition(pausedOnly, "pkg.alpha", "DialogAct", STOPPED);
            check(scheduler.scheduledCount() == 0 && deliveries.isEmpty(),
                    "PAUSED-only visibility counted toward foreground limit");

            // Session 1: pkg.alpha in foreground for 25,000ms -> single timer at 60,000ms
            Object a1 = new Object(), a2 = new Object(), b1 = new Object();
            svc.transition(a1, "pkg.alpha", "A1", RESUMED);
            check(scheduler.soleRemainingMs() == 60_000L,
                    "expected earliest deadline 60,000ms for two active observers");
            wall.addAndGet(25_000L); elapsed.addAndGet(25_000L);
            svc.transition(a1, "pkg.alpha", "A1", PAUSED);
            check(scheduler.scheduledCount() == 0, "timer remained armed after foreground pause");

            // Session 2: overlapping RESUMED activities across pkg.alpha (2 instances) and pkg.beta
            svc.transition(a1, "pkg.alpha", "A1", RESUMED);
            check(scheduler.soleRemainingMs() == 35_000L, "wrong remaining delay after 25s");
            wall.addAndGet(5_000L); elapsed.addAndGet(5_000L);
            svc.transition(a2, "pkg.alpha", "A2", RESUMED);
            svc.transition(b1, "pkg.beta", "B1", RESUMED);
            wall.addAndGet(15_000L); elapsed.addAndGet(15_000L);
            svc.transition(a1, "pkg.alpha", "A1", STOPPED);
            svc.transition(a2, "pkg.alpha", "A2", TERMINATED);
            // pkg.alpha left foreground (used 25s + 20s = 45s), but pkg.beta is STILL resumed!
            // Remaining to 60s for group-42 is 35,000 - 20,000 = 15,000ms!
            check(scheduler.soleRemainingMs() == 15_000L,
                    "group observer lost continuity or double-counted overlap");
            wall.addAndGet(5_000L); elapsed.addAndGet(5_000L);
            svc.transition(b1, "pkg.beta", "B1", PAUSED);
            // Total group-42 used = 25,000 + 25,000 = 50,000ms; alpha-99 used = 25,000 + 20,000 = 45,000ms
            check(deliveries.isEmpty() && scheduler.scheduledCount() == 0,
                    "premature delivery or lingering timer");

            // Session 3: resume pkg.alpha -> earliest is group-42 (10,000ms left), alpha-99 has 45,000ms left
            svc.transition(a1, "pkg.alpha", "A1", RESUMED);
            check(scheduler.soleRemainingMs() == 10_000L, "expected 10,000ms remaining for 42");
            wall.addAndGet(10_000L); elapsed.addAndGet(10_000L);
            scheduler.fireEarliest();

            check(deliveries.size() == 1 && am.sentCount == 1, "expected 1 AM PendingIntent send");
            Delivery first = deliveries.get(0);
            check(first.observerId() == 42 && first.timeLimit() == 60_000L
                    && first.timeUsed() == 60_000L && "group-42".equals(first.tag()),
                    "wrong extras on observer 42: " + first);

            // Immediately after observer 42 expires, observer 99 (35,000ms remaining) is armed alone!
            check(scheduler.soleRemainingMs() == 35_000L,
                    "next observer deadline 35,000ms was not armed after 42 expired");

            // Registering an observer while pkg.alpha is ALREADY in foreground starts immediately
            svc.registerAppUsageObserver(77, new String[] {"pkg.alpha"}, 60_000L,
                    recordingCallback(deliveries, "mid-session-77"), "com.observer.owner");
            check(scheduler.soleRemainingMs() == 35_000L,
                    "mid-session observer disturbed earlier 35,000ms deadline");

            wall.addAndGet(35_000L); elapsed.addAndGet(35_000L);
            scheduler.fireEarliest();
            check(deliveries.size() == 2 && deliveries.get(1).observerId() == 99
                    && deliveries.get(1).timeLimit() == 90_000L
                    && deliveries.get(1).timeUsed() == 90_000L,
                    "observer 99 did not fire at 90,000ms");
            // Now mid-session-77 has 25,000ms left!
            check(scheduler.soleRemainingMs() == 25_000L,
                    "mid-session-77 remaining 25,000ms not scheduled");

            // Uninstalling observer owner retires remaining observers and cancels the timer
            Binder.resetCallerForTest();
            pm.uninstall("com.observer.owner");
            check(svc.observers().observerCountForUid(10200) == 0
                    && scheduler.scheduledCount() == 0,
                    "uninstalled owner's observer or timer leaked");
        } finally {
            Binder.resetCallerForTest();
            svc.observers().close();
            svc.flush();
            Files.deleteIfExists(store);
            Files.deleteIfExists(tempDir);
        }
    }

    private static void realDaemonSchedulerZeroIdleWakeupsAndExpiry() throws Exception {
        PackageManagerService pm = new PackageManagerService();
        CallerIdentity callers = new CallerIdentity(pm, null);
        AtomicLong elapsed = new AtomicLong(1_000L);
        DesktopUsageStatsService svc = new DesktopUsageStatsService(
                pm, callers, null, elapsed::get, elapsed::get);
        List<Delivery> deliveries = new ArrayList<>();
        try {
            // Register 250 active observers on idle/paused apps and sleep 120ms: 0 wakeups, 0 queued tasks
            for (int i = 0; i < 250; i++) {
                svc.registerAppUsageObserver(i, new String[] {"idle.pkg." + i}, 60_000L,
                        recordingCallback(deliveries, "idle-" + i), "android");
            }
            Object pausedTok = new Object();
            svc.transition(pausedTok, "idle.pkg.0", "Act", RESUMED);
            elapsed.addAndGet(59_970L);
            svc.transition(pausedTok, "idle.pkg.0", "Act", PAUSED);
            check(svc.observers().scheduledTaskCount() == 0 && svc.observers().wakeupCount() == 0,
                    "paused observer left scheduled task or woke up");
            Thread.sleep(120L);
            check(svc.observers().wakeupCount() == 0 && svc.observers().scheduledTaskCount() == 0
                    && deliveries.isEmpty(),
                    "idle observers caused background wakeups");

            // Verify real ScheduledThreadPoolExecutor expiry & one-shot removal
            CountDownLatch latch = new CountDownLatch(1);
            PendingIntent liveCb = new PendingIntent((code, intent, type, tok, rec, perm, opts) -> {
                synchronized (deliveries) {
                    deliveries.add(new Delivery(
                            intent.getIntExtra(UsageStatsManager.EXTRA_OBSERVER_ID, -1),
                            intent.getLongExtra(UsageStatsManager.EXTRA_TIME_LIMIT, -1L),
                            intent.getLongExtra(UsageStatsManager.EXTRA_TIME_USED, -1L),
                            "real-timer"));
                }
                latch.countDown();
            });
            AtomicLong hybridBase = new AtomicLong(10_000L);
            final long[] liveAnchorNs = {0L};
            DesktopUsageStatsService hybrid = new DesktopUsageStatsService(
                    pm, callers, null,
                    () -> hybridBase.get() + (liveAnchorNs[0] == 0L ? 0L
                            : TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - liveAnchorNs[0])),
                    () -> hybridBase.get() + (liveAnchorNs[0] == 0L ? 0L
                            : TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - liveAnchorNs[0])));
            try {
                hybrid.registerAppUsageObserver(777, new String[] {"real.timer.pkg"}, 60_000L,
                        liveCb, "android");
                Object t = new Object();
                hybrid.transition(t, "real.timer.pkg", "Main", RESUMED);
                hybridBase.addAndGet(59_965L);
                hybrid.transition(t, "real.timer.pkg", "Main", PAUSED);
                liveAnchorNs[0] = System.nanoTime();
                hybrid.transition(t, "real.timer.pkg", "Main", RESUMED);
                check(hybrid.observers().scheduledTaskCount() == 1,
                        "expected 1 scheduled expiry task");
                check(latch.await(2, TimeUnit.SECONDS), "real scheduled expiry did not fire");
                check(hybrid.observers().wakeupCount() == 1
                        && hybrid.observers().scheduledTaskCount() == 0
                        && hybrid.observers().observerCountForUid(Process.SYSTEM_UID) == 0,
                        "expected exactly 1 wakeup and one-shot auto-unregistration");
                synchronized (deliveries) {
                    Delivery fired = deliveries.get(deliveries.size() - 1);
                    check(fired.observerId() == 777 && fired.timeLimit() == 60_000L
                            && fired.timeUsed() >= 60_000L,
                            "real timer callback carried wrong extras: " + fired);
                }
            } finally {
                hybrid.observers().close();
            }
        } finally {
            svc.observers().close();
        }
    }

    public static void main(String[] args) throws Exception {
        permissionAndValidationContracts();
        perUidCapAndReplacementContracts();
        foregroundAccumulationAndSingleTimerContracts();
        realDaemonSchedulerZeroIdleWakeupsAndExpiry();
        System.out.println("UsageLimitObserversContractTest: " + checks + " checks passed");
    }
}
"""

BENCHMARK_HARNESS = r"""package com.gesher.server;

import static com.gesher.server.ActivityUsageHistory.State.PAUSED;
import static com.gesher.server.ActivityUsageHistory.State.RESUMED;
import static com.gesher.server.ActivityUsageHistory.State.STOPPED;
import static com.gesher.server.ActivityUsageHistory.State.TERMINATED;

import android.app.PendingIntent;
import java.lang.management.ManagementFactory;
import java.lang.management.ThreadMXBean;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicLong;

public final class UsageTransitionBench {
    private static long threadAllocatedBytes(ThreadMXBean bean, long tid) {
        try {
            Method m = Class.forName("com.sun.management.ThreadMXBean").getMethod("getThreadAllocatedBytes", long.class);
            m.setAccessible(true);
            return (Long) m.invoke(bean, tid);
        } catch (Throwable ignored) {
            return -1L;
        }
    }

    public static void main(String[] args) throws Exception {
        boolean candidate = "candidate".equals(args[0]);
        Path dir = Files.createTempDirectory("gesher-abba-usage");
        Path store = dir.resolve("activity-usage.bin");
        PackageManagerService pm = new PackageManagerService();
        CallerIdentity callers = new CallerIdentity(pm, null);
        AtomicLong wall = new AtomicLong(1_700_000_000_000L);
        AtomicLong elapsed = new AtomicLong(1_000_000L);
        DesktopUsageStatsService svc = new DesktopUsageStatsService(
                pm, callers, store, wall::get, elapsed::get);

        AutoCloseable historyClose = null;
        AutoCloseable obsClose = null;
        try {
            var hf = svc.getClass().getDeclaredField("mHistory");
            hf.setAccessible(true);
            historyClose = (AutoCloseable) hf.get(svc);
            if (candidate) {
                Method obsGetter = svc.getClass().getDeclaredMethod("observers");
                obsGetter.setAccessible(true);
                obsClose = (AutoCloseable) obsGetter.invoke(svc);
                PendingIntent noop = new PendingIntent(
                        (c, i, r, w, f, p, o) -> {});
                for (int i = 0; i < 500; i++) {
                    svc.registerAppUsageObserver(i, new String[] {"watched.idle." + i},
                            600_000L, noop, "android");
                }
            }

            // 1. Measure idle background wakeups over 200ms with 500 registered observers
            long idleWakeups = 0L;
            int idleScheduled = 0;
            int fgScheduled = 0;
            long fgIdleWakeups = 0L;
            if (candidate) {
                Method wakeupM = obsClose.getClass().getDeclaredMethod("wakeupCount");
                Method schedM = obsClose.getClass().getDeclaredMethod("scheduledTaskCount");
                wakeupM.setAccessible(true);
                schedM.setAccessible(true);
                Thread.sleep(150L);
                idleWakeups = (Long) wakeupM.invoke(obsClose);
                idleScheduled = (Integer) schedM.invoke(obsClose);

                // Also arm 1 foreground observed package (10-min limit) for 150ms -> 1 task, 0 wakeups
                Object fgTok = new Object();
                svc.transition(fgTok, "watched.idle.0", "WatchedAct", RESUMED);
                fgScheduled = (Integer) schedM.invoke(obsClose);
                Thread.sleep(150L);
                fgIdleWakeups = (Long) wakeupM.invoke(obsClose);
                svc.transition(fgTok, "watched.idle.0", "WatchedAct", STOPPED);
            } else {
                Thread.sleep(300L);
            }

            // 2. Warm up transition(...) path (50,000 transitions = 12,500 full cycles)
            Object[] tokens = new Object[8];
            String[] pkgs = new String[8];
            for (int i = 0; i < tokens.length; i++) {
                tokens[i] = new Object();
                pkgs[i] = "bench.app." + i;
            }
            for (int i = 0; i < 12_500; i++) {
                int idx = i & 7;
                Object tok = tokens[idx];
                String pkg = pkgs[idx];
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", RESUMED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", PAUSED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", STOPPED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", TERMINATED);
            }

            // 3. Measure 200,000 transitions (50,000 full 4-state cycles)
            final int cycles = 50_000;
            final int totalTransitions = cycles * 4;
            ThreadMXBean bean = ManagementFactory.getThreadMXBean();
            long tid = Thread.currentThread().threadId();
            long allocBefore = threadAllocatedBytes(bean, tid);
            long cpuBefore = bean.getCurrentThreadCpuTime();
            long nsBefore = System.nanoTime();

            for (int i = 0; i < cycles; i++) {
                int idx = i & 7;
                Object tok = tokens[idx];
                String pkg = pkgs[idx];
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", RESUMED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", PAUSED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", STOPPED);
                wall.addAndGet(5L); elapsed.addAndGet(5L);
                svc.transition(tok, pkg, "MainActivity", TERMINATED);
            }

            long nsElapsed = System.nanoTime() - nsBefore;
            long cpuElapsed = bean.getCurrentThreadCpuTime() - cpuBefore;
            long allocAfter = threadAllocatedBytes(bean, tid);
            long allocBytes = (allocBefore >= 0 && allocAfter >= 0) ? (allocAfter - allocBefore) : -1L;

            System.out.printf(
                    "{\"role\":\"%s\",\"transitions\":%d,\"elapsed_ms\":%.3f,\"ns_per_transition\":%.2f,"
                    + "\"cpu_ms\":%.3f,\"alloc_bytes\":%d,\"bytes_per_transition\":%.2f,"
                    + "\"idle_wakeups\":%d,\"idle_scheduled\":%d,\"fg_scheduled\":%d,\"fg_idle_wakeups\":%d}%n",
                    args[0], totalTransitions, nsElapsed / 1_000_000.0,
                    (double) nsElapsed / totalTransitions, cpuElapsed / 1_000_000.0,
                    allocBytes, allocBytes >= 0 ? (double) allocBytes / totalTransitions : -1.0,
                    idleWakeups, idleScheduled, fgScheduled, fgIdleWakeups);
        } finally {
            if (obsClose != null) obsClose.close();
            if (historyClose != null) historyClose.close();
            Files.deleteIfExists(store);
            Files.deleteIfExists(dir);
        }
    }
}
"""


def write_tree(base: Path, files: dict[str, str]) -> list[str]:
    paths: list[str] = []
    for rel, content in files.items():
        target = base / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        paths.append(str(target))
    return paths


def compile_variant(javac: str, work: Path, ref: str | None, include_contract_test: bool) -> Path:
    src_dir = work / ("src_" + (ref or "cand"))
    cls_dir = work / ("cls_" + (ref or "cand"))
    cls_dir.mkdir(parents=True, exist_ok=True)
    inputs = write_tree(src_dir, STUBS)

    server_files = ["ActivityUsageHistory.java", "DesktopUsageStatsService.java", "CallerIdentity.java"]
    if ref is None:
        server_files.insert(1, "UsageLimitObservers.java")

    for name in server_files:
        rel = "app/java/com/gesher/server/" + name
        if ref is None:
            content = (ROOT / rel).read_text(encoding="utf-8")
        else:
            content = subprocess.run(
                ["git", "show", f"{ref}:{rel}"],
                cwd=ROOT, check=True, capture_output=True, text=True
            ).stdout
        dst = src_dir / "com/gesher/server" / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(content, encoding="utf-8")
        inputs.append(str(dst))

    bench = src_dir / "com/gesher/server/UsageTransitionBench.java"
    bench.write_text(BENCHMARK_HARNESS, encoding="utf-8")
    inputs.append(str(bench))

    if include_contract_test:
        test_file = src_dir / "com/gesher/server/UsageLimitObserversContractTest.java"
        test_file.write_text(CONTRACT_HARNESS, encoding="utf-8")
        inputs.append(str(test_file))

    subprocess.run(
        [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-d", str(cls_dir), *inputs],
        check=True, timeout=60
    )
    return cls_dir


def run_bench(java: str, cls_dir: Path, role: str) -> dict:
    out = subprocess.run(
        [java, "-XX:-UsePerfData", "-Xms128m", "-Xmx128m", "-cp", str(cls_dir),
         "com.gesher.server.UsageTransitionBench", role],
        check=True, capture_output=True, text=True, timeout=90
    ).stdout.strip().splitlines()[-1]
    return json.loads(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--abba-baseline-ref", help="Run deterministic ABBA (cand1, base1, base2, cand2) against git ref")
    args = parser.parse_args()

    javac = str(args.java_home / "bin/javac")
    java = str(args.java_home / "bin/java")

    with tempfile.TemporaryDirectory(prefix="gesher-usage-observers-") as tmp:
        work = Path(tmp)
        cand_cls = compile_variant(javac, work, None, include_contract_test=True)
        subprocess.run(
            [java, "-XX:-UsePerfData", "-cp", str(cand_cls),
             "com.gesher.server.UsageLimitObserversContractTest"],
            check=True, timeout=60
        )

        if args.abba_baseline_ref:
            base_cls = compile_variant(javac, work, args.abba_baseline_ref, include_contract_test=False)
            order = [
                ("cand1", cand_cls, "candidate"),
                ("base1", base_cls, "baseline"),
                ("base2", base_cls, "baseline"),
                ("cand2", cand_cls, "candidate"),
            ]
            results = []
            for label, cp, role in order:
                row = run_bench(java, cp, role)
                row["run"] = label
                results.append(row)
                print("ABBA_ROW " + json.dumps(row), flush=True)

            for r in results:
                if r["role"] == "candidate":
                    if r["idle_wakeups"] != 0 or r["fg_idle_wakeups"] != 0:
                        raise AssertionError(f"Non-zero idle wakeups in {r['run']}: {r}")
                    if r["idle_scheduled"] != 0 or r["fg_scheduled"] != 1:
                        raise AssertionError(f"Unexpected scheduled task count in {r['run']}: {r}")


if __name__ == "__main__":
    main()
