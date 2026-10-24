/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/**
 * PendingIntents, lite (PendingIntentController and PendingIntentRecord): the system
 * server keeps one IIntentSender per key (type, package, request code, the intent as
 * {@code Intent.filterEquals} sees it), as PendingIntent.getActivity/getBroadcast/getService
 * ask for, with FLAG_NO_CREATE, FLAG_CANCEL_CURRENT, FLAG_UPDATE_CURRENT and FLAG_ONE_SHOT.
 * Sending one starts its activity, sends its broadcast or starts its service as its
 * creator would. Real apps create them as they start (alarms, notifications, sync
 * schedulers; docs/e2e.md); before, getIntentSenderWithFeature answered null and
 * AlarmManager.cancel(null) threw.
 */
final class PendingIntents {
    private final ActivityManagerService mAm;
    private final List<Record> mRecords = new ArrayList<>();

    PendingIntents(ActivityManagerService am) {
        mAm = am;
    }

    /** A PendingIntent's target in the system server. */
    final class Record extends IIntentSender.Stub {
        final int type;
        final String packageName;
        final int requestCode;
        final int flags;
        final int uid;
        Intent[] intents;
        String[] resolvedTypes;
        volatile boolean canceled;

        Record(int type, String packageName, int requestCode, Intent[] intents,
                String[] resolvedTypes, int flags, int uid) {
            this.type = type;
            this.packageName = packageName;
            this.requestCode = requestCode;
            this.intents = intents;
            this.resolvedTypes = resolvedTypes;
            this.flags = flags;
            this.uid = uid;
        }

        boolean sameKey(int type, String packageName, int requestCode, Intent intent) {
            return this.type == type && Objects.equals(this.packageName, packageName)
                    && this.requestCode == requestCode && last().filterEquals(intent);
        }

        Intent last() {
            return intents[intents.length - 1];
        }

        @Override
        public void send(int code, Intent fillIn, String resolvedType, IBinder allowlistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            PendingIntents.this.send(this, fillIn, resolvedType);
        }

        @Override
        public String toString() {
            return "PendingIntent{" + typeName(type) + " " + packageName + " "
                    + last().toShortString(false, true, true, false) + "}";
        }
    }

    IIntentSender get(int type, String packageName, int requestCode, Intent[] intents,
            String[] resolvedTypes, int flags) {
        if (intents == null || intents.length == 0) return null;
        Intent[] copies = new Intent[intents.length];
        for (int i = 0; i < intents.length; i++) copies[i] = new Intent(intents[i]);
        boolean noCreate = (flags & PendingIntent.FLAG_NO_CREATE) != 0;
        boolean cancelCurrent = (flags & PendingIntent.FLAG_CANCEL_CURRENT) != 0;
        boolean updateCurrent = (flags & PendingIntent.FLAG_UPDATE_CURRENT) != 0;
        synchronized (mRecords) {
            Record existing = null;
            for (Record r : mRecords) {
                if (!r.canceled && r.sameKey(type, packageName, requestCode, copies[copies.length - 1])) {
                    existing = r;
                    break;
                }
            }
            if (existing != null) {
                if (cancelCurrent) {
                    existing.canceled = true;
                    mRecords.remove(existing);
                } else {
                    if (updateCurrent) {
                        existing.intents = copies;
                        existing.resolvedTypes = resolvedTypes;
                    }
                    return existing;
                }
            }
            if (noCreate) return null;
            Record r = new Record(type, packageName, requestCode, copies, resolvedTypes, flags,
                    Binder.getCallingUid());
            mRecords.add(r);
            return r;
        }
    }

    Record record(IIntentSender sender) {
        return sender instanceof Record ? (Record) sender : null;
    }

    void cancel(IIntentSender sender) {
        Record r = record(sender);
        if (r == null) return;
        r.canceled = true;
        synchronized (mRecords) {
            mRecords.remove(r);
        }
    }

    /** Fires {@code r} with {@code fillIn} merged into its intent. Returns START_* or 0. */
    int send(Record r, Intent fillIn, String resolvedType) {
        if (r == null || r.canceled) return ActivityManager.START_CANCELED;
        if ((r.flags & PendingIntent.FLAG_ONE_SHOT) != 0) cancel(r);
        Intent intent = new Intent(r.last());
        if (fillIn != null && (r.flags & PendingIntent.FLAG_IMMUTABLE) == 0) {
            intent.fillIn(fillIn, 0);
        }
        String type = r.resolvedTypes != null ? r.resolvedTypes[r.resolvedTypes.length - 1]
                : resolvedType;
        SystemServer.log("send " + r);
        long ident = Binder.clearCallingIdentity();
        try {
            switch (r.type) {
                case ActivityManager.INTENT_SENDER_BROADCAST:
                    return mAm.sendBroadcastFor(r.packageName, intent, type);
                case ActivityManager.INTENT_SENDER_ACTIVITY:
                    return mAm.startActivityFor(r.packageName, intent, type);
                case ActivityManager.INTENT_SENDER_SERVICE:
                case ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE:
                    mAm.startServiceFor(r.packageName, intent, type,
                            r.type == ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE);
                    return 0;
                default:
                    return 0;
            }
        } finally {
            Binder.restoreCallingIdentity(ident);
        }
    }

    static String typeName(int type) {
        switch (type) {
            case ActivityManager.INTENT_SENDER_BROADCAST: return "broadcast";
            case ActivityManager.INTENT_SENDER_ACTIVITY: return "activity";
            case ActivityManager.INTENT_SENDER_ACTIVITY_RESULT: return "activity result";
            case ActivityManager.INTENT_SENDER_SERVICE: return "service";
            case ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE: return "foreground service";
            default: return "type " + type;
        }
    }
}
