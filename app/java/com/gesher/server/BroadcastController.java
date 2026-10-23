/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.CompatibilityInfo;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Broadcasts, lite (BroadcastController and BroadcastQueue): receivers registered at run
 * time ({@code registerReceiver}) and the manifest's {@code <receiver>}s, delivered in
 * order, one intent at a time, not ordered (no results, no abort), not sticky.
 *
 * <ul>
 * <li>A registered receiver gets an implicit broadcast its filter matches, through its
 *     process's {@code scheduleRegisteredReceiver} (→ the LoadedApk ReceiverDispatcher →
 *     onReceive on the main thread).
 * <li>A manifest receiver gets a broadcast naming it (explicit), or an implicit one sent
 *     to its package ({@code setPackage}) that its filter matches, as Android delivers
 *     implicit broadcasts to manifest receivers only then: {@code scheduleReceiver} (→
 *     handleReceiver: a new instance, onReceive, {@code finishReceiver}).
 * </ul>
 */
final class BroadcastController {
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;

    /** A registered receiver: ReceiverList, with its filters. */
    private static final class ReceiverList {
        final IIntentReceiver receiver;
        final ActivityTaskManagerService.ProcessRecord app;
        final String packageName;
        final List<IntentFilter> filters = new ArrayList<>();

        ReceiverList(IIntentReceiver receiver, ActivityTaskManagerService.ProcessRecord app,
                String packageName) {
            this.receiver = receiver;
            this.app = app;
            this.packageName = packageName;
        }
    }

    // Guarded by this.
    private final Map<IBinder, ReceiverList> mRegistered = new LinkedHashMap<>();

    BroadcastController(ActivityTaskManagerService atm, PackageManagerService pm) {
        mAtm = atm;
        mPm = pm;
    }

    Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, int callingPid) {
        if (receiver == null) return null;  // a query for sticky broadcasts: there are none
        ActivityTaskManagerService.ProcessRecord app = mAtm.callerProcess(callingPid);
        if (app == null) {
            SystemServer.log("registerReceiver from pid " + callingPid
                    + ": not an attached process");
            return null;
        }
        synchronized (this) {
            ReceiverList rl = mRegistered.get(receiver.asBinder());
            if (rl == null) {
                rl = new ReceiverList(receiver, app, callerPackage);
                mRegistered.put(receiver.asBinder(), rl);
            }
            rl.filters.add(new IntentFilter(filter));
        }
        StringBuilder actions = new StringBuilder();
        for (int i = 0; i < filter.countActions(); i++) {
            if (i > 0) actions.append(", ");
            actions.append(filter.getAction(i));
        }
        SystemServer.log("registerReceiver from " + callerPackage + ": " + actions);
        return null;  // no sticky broadcast matches
    }

    void unregisterReceiver(IIntentReceiver receiver) {
        ReceiverList rl;
        synchronized (this) {
            rl = mRegistered.remove(receiver.asBinder());
        }
        if (rl != null) SystemServer.log("unregisterReceiver of " + rl.packageName);
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid) {
        intent = new Intent(intent);
        // Registered receivers: implicit broadcasts only.
        List<ReceiverList> registered = new ArrayList<>();
        if (intent.getComponent() == null) {
            synchronized (this) {
                for (ReceiverList rl : mRegistered.values()) {
                    if (intent.getPackage() != null && !intent.getPackage().equals(rl.packageName)) {
                        continue;
                    }
                    for (IntentFilter f : rl.filters) {
                        if (f.match(intent.getAction(), resolvedType != null ? resolvedType
                                : intent.getType(), intent.getScheme(), intent.getData(),
                                intent.getCategories(), "gesher") >= 0) {
                            registered.add(rl);
                            break;
                        }
                    }
                }
            }
        }
        // Manifest receivers: explicit, or implicit to a package.
        List<ActivityInfo> manifest = new ArrayList<>();
        if (intent.getComponent() != null || intent.getPackage() != null) {
            for (ResolveInfo ri : mPm.receivers(intent, resolvedType,
                    PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                manifest.add(ri.activityInfo);
            }
        }
        SystemServer.log("broadcastIntent " + intent.toShortString(false, true, true, false)
                + " from " + callingPackage + ": " + registered.size() + " registered, "
                + manifest.size() + " manifest receiver(s)");
        int sendingUid = Process.myUid();  // every gesher process has the same uid
        for (ReceiverList rl : registered) {
            try {
                rl.app.thread.scheduleRegisteredReceiver(rl.receiver, intent,
                        android.app.Activity.RESULT_OK, null /* data */, null /* extras */,
                        false /* ordered */, false /* sticky */, true /* assumeDelivered */,
                        0 /* sendingUser */, ActivityManager.PROCESS_STATE_TOP, sendingUid,
                        callingPackage);
            } catch (RemoteException e) {
                SystemServer.log("scheduleRegisteredReceiver: " + e);
            }
        }
        for (ActivityInfo info : manifest) {
            Intent delivered = new Intent(intent);
            delivered.setComponent(new android.content.ComponentName(info.packageName,
                    info.name));
            mAtm.withProcess(info.packageName, "broadcast to " + info.name, app -> {
                SystemServer.log("scheduleReceiver " + info.packageName + "/" + info.name
                        + " in " + app);
                try {
                    app.thread.scheduleReceiver(delivered, info,
                            CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                            android.app.Activity.RESULT_OK, null /* data */,
                            null /* extras */, false /* ordered */,
                            false /* assumeDelivered */, 0 /* sendingUser */,
                            ActivityManager.PROCESS_STATE_RECEIVER, sendingUid,
                            callingPackage);
                } catch (RemoteException e) {
                    SystemServer.log("scheduleReceiver: " + e);
                }
            });
        }
        return ActivityManager.BROADCAST_SUCCESS;
    }

    void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map,
            boolean abortBroadcast, int flags) {
        // Not ordered: nothing waits for a receiver to finish.
    }

    /** The process went away: so did its registered receivers. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        synchronized (this) {
            mRegistered.values().removeIf(rl -> rl.app == app);
        }
    }
}
