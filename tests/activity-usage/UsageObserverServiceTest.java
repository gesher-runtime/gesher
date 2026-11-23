/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.PendingIntent;
import android.app.usage.UsageStatsManager;
import android.content.Intent;
import android.os.Binder;
import android.os.RemoteException;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

import static com.gesher.server.UsageLimitObserversTest.check;
import static com.gesher.server.UsageLimitObserversTest.throwsType;

/** Compiles the complete service/history/observer sources over explicit Android I/O boundaries. */
public final class UsageObserverServiceTest {
    public static void main(String[] args) throws Exception {
        Path directory = Files.createTempDirectory("gesher-observer-service-");
        PackageManagerService packages = new PackageManagerService();
        ApkPackage owner = new ApkPackage("owner", 10001);
        packages.installed.put("owner", owner);
        CallerIdentity callers = new CallerIdentity(packages, owner);
        AtomicLong clock = new AtomicLong(1000);
        UsageLimitObserversTest.ManualScheduler timer = new UsageLimitObserversTest.ManualScheduler(clock);
        DesktopUsageStatsService service = new DesktopUsageStatsService(packages, callers,
                directory.resolve("history.bin"), clock::get, clock::get, timer);
        List<Intent> delivered = new ArrayList<>();
        PendingIntent callback = new PendingIntent((code, intent, type, whitelist, receiver, permission, options) -> {
            check(code == 0 && type == null && permission == null, "Wrong callback send parameters");
            check(Binder.cleared, "Callback retained the app's Binder identity");
            delivered.add(intent);
        });
        try {
            throwsType(SecurityException.class, () -> service.registerAppUsageObserver(
                    1, new String[]{"app"}, 60000, callback, "owner"));
            throwsType(SecurityException.class, () -> service.unregisterAppUsageObserver(1, "owner"));
            packages.permissions.granted = true;
            throwsType(SecurityException.class, () -> service.registerAppUsageObserver(
                    1, new String[]{"app"}, 60000, callback, "foreign"));
            throwsType(SecurityException.class, () -> service.unregisterAppUsageObserver(1, "foreign"));
            throwsType(IllegalArgumentException.class, () -> service.registerAppUsageObserver(
                    1, null, 60000, callback, "owner"));
            throwsType(IllegalArgumentException.class, () -> service.registerAppUsageObserver(
                    1, new String[0], 60000, callback, "owner"));
            throwsType(NullPointerException.class, () -> service.registerAppUsageObserver(
                    1, new String[]{"app"}, 60000, null, "owner"));
            throwsType(IllegalArgumentException.class, () -> service.registerAppUsageObserver(
                    1, new String[]{"app"}, 59999, callback, "owner"));
            check(Manifest.permission.OBSERVE_APP_USAGE.equals(packages.permissions.lastPermission),
                    "Wrong observer permission checked");
            check(packages.permissions.lastUid == owner.uid() && packages.permissions.lastPid == callers.pid,
                    "Observer permission was not checked against Binder identity");

            service.registerAppUsageObserver(73, new String[]{"root"}, 60000, callback, "owner");
            service.registerAppUsageObserver(74, new String[]{"child"}, 60000, callback, "owner");
            Binder activity = new Binder();
            service.transition(activity, "child", "Activity", "root", ActivityUsageHistory.State.PAUSED);
            timer.advance(61000);
            check(delivered.size() == 1, "PAUSED-first task-root usage was not observed");
            Intent result = delivered.getFirst();
            check(result.getIntExtra(UsageStatsManager.EXTRA_OBSERVER_ID, -1) == 73,
                    "Callback observer ID missing or changed");
            check(result.getLongExtra(UsageStatsManager.EXTRA_TIME_LIMIT, -1) == 60000
                            && result.getLongExtra(UsageStatsManager.EXTRA_TIME_USED, -1) == 60000,
                    "Callback millisecond time extras missing or changed");
            check(result.extras.size() == 3 && !Binder.cleared, "Callback identity/extras were not restored");
            service.transition(activity, "child", "Activity", "root", ActivityUsageHistory.State.STOPPED);
            service.unregisterAppUsageObserver(74, "owner");
            service.registerAppUsageObserver(74, new String[]{"child"}, 60000, callback, "owner");
            service.transition(activity, "child", "Activity", ActivityUsageHistory.State.RESUMED);
            clock.set(81000);
            service.transition(activity, "child", "Activity", ActivityUsageHistory.State.PAUSED);
            timer.advance(121000);
            check(delivered.size() == 2, "PAUSED activity prematurely stopped observation");
            service.transition(activity, "child", "Activity", ActivityUsageHistory.State.TERMINATED);

            // Reported feature tokens are in the authenticated caller's namespace.
            service.registerAppUsageObserver(80, new String[]{"owner/feature"}, 60000, callback, "owner");
            service.reportPastUsageStart(activity, "feature", 30000, "owner");
            throwsType(SecurityException.class, () -> service.reportUsageStop(activity, "feature", "foreign"));
            timer.advance(151000);
            check(delivered.size() == 3 && delivered.getLast().getIntExtra(
                    UsageStatsManager.EXTRA_OBSERVER_ID, -1) == 80, "Reported token did not reach observer");
            service.reportUsageStop(activity, "feature", "owner");
            throwsType(IllegalArgumentException.class, () -> service.reportUsageStop(activity, "feature", "owner"));

            // Removal suppresses old installation callbacks and forbids new admission.
            service.registerAppUsageObserver(90, new String[]{"child"}, 60000, callback, "owner");
            service.transition(activity, "child", "Activity", ActivityUsageHistory.State.RESUMED);
            packages.pending.add("owner");
            for (var listener : packages.removals) listener.accept("owner");
            throwsType(SecurityException.class, () -> service.registerAppUsageObserver(
                    91, new String[]{"child"}, 60000, callback, "owner"));
            timer.advance(211000);
            check(delivered.size() == 3 && timer.pending() == 0, "Removed observer's callback leaked");
            packages.pending.clear();
            service.registerAppUsageObserver(92, new String[]{"child"}, 60000, callback, "owner");
            packages.installed.put("owner", new ApkPackage("owner", owner.uid()));
            timer.advance(271000);
            check(delivered.size() == 3, "Replacement received previous installation's callback");
            packages.installed.put("owner", owner);

            PendingIntent dead = new PendingIntent((code, intent, type, whitelist, receiver, permission, options) -> {
                throw new RemoteException();
            });
            service.registerAppUsageObserver(94, new String[]{"child"}, 60000, dead, "owner");
            service.registerAppUsageObserver(95, new String[]{"child"}, 60000, callback, "owner");
            timer.advance(331000);
            check(delivered.size() == 4 && delivered.getLast().getIntExtra(
                    UsageStatsManager.EXTRA_OBSERVER_ID, -1) == 95, "Dead callback blocked another observer");
            check(!Binder.cleared, "Failed send did not restore identity");

            throwsType(SecurityException.class, () -> service.registerUsageSessionObserver(
                    1, new String[]{"child"}, 60000, 1, callback, callback, "owner"));
            throwsType(SecurityException.class, () -> service.registerAppUsageLimitObserver(
                    1, new String[]{"child"}, 60000, 1, callback, "owner"));
            DesktopUsageStatsService unmanaged = new DesktopUsageStatsService(packages, callers,
                    null, clock::get, clock::get, timer);
            throwsType(UnsupportedOperationException.class, () -> unmanaged.registerAppUsageObserver(
                    1, new String[]{"app"}, 60000, callback, "owner"));
            unmanaged.unregisterAppUsageObserver(1, "owner");
            throwsType(UnsupportedOperationException.class, () -> unmanaged.reportUsageStart(activity, "feature", "owner"));
            System.out.println("UsageObserverServiceTest passed: identity, permission, lifecycle, callbacks, retirement");
        } finally {
            var field = DesktopUsageStatsService.class.getDeclaredField("mHistory");
            field.setAccessible(true);
            ((ActivityUsageHistory) field.get(service)).close();
            try (var files = Files.list(directory)) { for (Path file : files.toList()) Files.delete(file); }
            Files.delete(directory);
        }
    }
}
