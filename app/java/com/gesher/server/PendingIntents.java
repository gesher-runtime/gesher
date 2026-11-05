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
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final java.util.Map<ApkPackage, String> mCodeIdentities = new java.util.WeakHashMap<>();
    private final List<Record> mRecords = new ArrayList<>();

    PendingIntents(ActivityManagerService am, PackageManagerService pm) {
        mAm = am;
        mPm = pm;
        mCallers = am.callers();
    }

    /** A PendingIntent's target in the system server. */
    final class Record extends IIntentSender.Stub {
        final int type;
        final String packageName;
        final int requestCode;
        final int flags;
        final int uid;
        final String creatorIdentity;
        Intent[] intents;
        String[] resolvedTypes;
        volatile boolean canceled;

        Record(int type, String packageName, int requestCode, Intent[] intents,
                String[] resolvedTypes, int flags, int uid, String creatorIdentity) {
            this.type = type;
            this.packageName = packageName;
            this.requestCode = requestCode;
            this.intents = intents;
            this.resolvedTypes = resolvedTypes;
            this.flags = flags;
            this.uid = uid;
            this.creatorIdentity = creatorIdentity;
        }

        boolean sameKey(int type, String packageName, int uid, String identity,
                int requestCode, Intent intent, int flags) {
            return this.type == type && Objects.equals(this.packageName, packageName)
                    && this.uid == uid && creatorIdentity.equals(identity)
                    && (this.flags & (PendingIntent.FLAG_ONE_SHOT | PendingIntent.FLAG_IMMUTABLE
                            | PendingIntent.FLAG_MUTABLE)) == (flags & (PendingIntent.FLAG_ONE_SHOT
                                    | PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_MUTABLE))
                    && this.requestCode == requestCode && last().filterEquals(intent);
        }

        Intent last() {
            return intents[intents.length - 1];
        }

        @Override
        public void send(int code, Intent fillIn, String resolvedType, IBinder allowlistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            PendingIntents.this.send(this, fillIn, resolvedType, requiredPermission);
        }

        @Override
        public String toString() {
            return "PendingIntent{" + typeName(type) + " " + packageName + " "
                    + last().toShortString(false, true, true, false) + "}";
        }
    }

    IIntentSender get(int type, String packageName, int requestCode, Intent[] intents,
            String[] resolvedTypes, int flags) {
        mCallers.requirePackage(packageName);
        ApkPackage creator = mPm.get(packageName);
        if (creator == null && !(mCallers.isSystem() && "android".equals(packageName))) {
            throw new SecurityException("Unknown PendingIntent creator");
        }
        int uid = creator == null ? android.os.Process.SYSTEM_UID : creator.uid();
        String identity = creator == null ? "android-system" : codeIdentity(creator);
        if ((flags & PendingIntent.FLAG_IMMUTABLE) != 0 && (flags & PendingIntent.FLAG_MUTABLE) != 0) {
            throw new IllegalArgumentException("Conflicting PendingIntent mutability flags");
        }
        if (intents == null || intents.length == 0) return null;
        if (resolvedTypes != null && resolvedTypes.length != intents.length) {
            throw new IllegalArgumentException("PendingIntent type array does not match its intents");
        }
        Intent[] copies = new Intent[intents.length];
        for (int i = 0; i < intents.length; i++) copies[i] = new Intent(intents[i]);
        boolean noCreate = (flags & PendingIntent.FLAG_NO_CREATE) != 0;
        boolean cancelCurrent = (flags & PendingIntent.FLAG_CANCEL_CURRENT) != 0;
        boolean updateCurrent = (flags & PendingIntent.FLAG_UPDATE_CURRENT) != 0;
        synchronized (mRecords) {
            Record existing = null;
            for (Record r : mRecords) {
                if (!r.canceled && r.sameKey(type, packageName, uid, identity, requestCode,
                        copies[copies.length - 1], flags)) {
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
                        existing.resolvedTypes = resolvedTypes == null ? null : resolvedTypes.clone();
                    }
                    return existing;
                }
            }
            if (noCreate) return null;
            Record r = new Record(type, packageName, requestCode, copies,
                    resolvedTypes == null ? null : resolvedTypes.clone(), flags, uid, identity);
            mRecords.add(r);
            return r;
        }
    }

    Record record(IIntentSender sender) {
        if (!(sender instanceof Record)) return null;
        synchronized (mRecords) { return mRecords.contains(sender) ? (Record) sender : null; }
    }

    void cancel(IIntentSender sender) {
        Record r = record(sender);
        if (r == null) return;
        requireCreator(r);
        synchronized (mRecords) {
            r.canceled = true;
            mRecords.remove(r);
        }
    }

    void requireCreator(Record r) {
        if (!mCallers.isSystem()) {
            mCallers.requirePackage(r.packageName);
            if (mCallers.callerUid() != r.uid) throw new SecurityException("PendingIntent creator UID changed");
        }
    }

    /** Fires {@code r} with {@code fillIn} merged into its intent. Returns START_* or 0. */
    int send(Record r, Intent fillIn, String resolvedType) {
        return send(r, fillIn, resolvedType, null);
    }

    int send(Record r, Intent fillIn, String resolvedType, String requiredPermission) {
        final Intent intent;
        final String type;
        synchronized (mRecords) {
            if (r == null || r.canceled || !mRecords.contains(r)) return ActivityManager.START_CANCELED;
            ApkPackage current = mPm.get(r.packageName);
            boolean valid = current == null ? "android".equals(r.packageName)
                    && r.uid == android.os.Process.SYSTEM_UID && "android-system".equals(r.creatorIdentity)
                    : current.uid() == r.uid && r.creatorIdentity.equals(codeIdentity(current));
            if (!valid) {
                r.canceled = true; mRecords.remove(r); return ActivityManager.START_CANCELED;
            }
            intent = new Intent(r.last());
            type = r.resolvedTypes != null ? r.resolvedTypes[r.resolvedTypes.length - 1] : resolvedType;
            // Sending is capability based: a legitimate recipient can consume a
            // one-shot record, but cannot separately cancel or update its creator's PI.
            if ((r.flags & PendingIntent.FLAG_ONE_SHOT) != 0) {
                r.canceled = true; mRecords.remove(r);
            }
        }
        if (fillIn != null && (r.flags & PendingIntent.FLAG_IMMUTABLE) == 0) {
            intent.fillIn(fillIn, 0);
        }
        SystemServer.log("send " + r);
        long ident = Binder.clearCallingIdentity();
        try {
            switch (r.type) {
                case ActivityManager.INTENT_SENDER_BROADCAST:
                    return mAm.sendBroadcastFor(r.packageName, intent, type, requiredPermission);
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

    private String codeIdentity(ApkPackage pkg) {
        synchronized (mCodeIdentities) {
            String cached = mCodeIdentities.get(pkg);
            if (cached != null) return cached;
            try {
                java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
                android.content.pm.Signature[] signers = pkg.verifiedSignatures();
                if (signers != null) {
                    for (android.content.pm.Signature signer : signers) {
                        byte[] bytes = signer.toByteArray();
                        digest.update(java.nio.ByteBuffer.allocate(4).putInt(bytes.length).array());
                        digest.update(bytes);
                    }
                } else {
                    java.util.List<java.io.File> files = new java.util.ArrayList<>();
                    files.add(new java.io.File(pkg.apkPath));
                    android.content.pm.ApplicationInfo info = pkg.applicationInfo(0);
                    if (info.splitSourceDirs != null) for (String split : info.splitSourceDirs) {
                        files.add(new java.io.File(split));
                    }
                    files.sort(java.util.Comparator.comparing(java.io.File::getName));
                    byte[] buffer = new byte[32768];
                    for (java.io.File file : files) {
                        byte[] name = file.getName().getBytes(java.nio.charset.StandardCharsets.UTF_8);
                        digest.update(java.nio.ByteBuffer.allocate(4).putInt(name.length).array());
                        digest.update(name);
                        digest.update(java.nio.ByteBuffer.allocate(8).putLong(file.length()).array());
                        try (java.io.FileInputStream input = new java.io.FileInputStream(file)) {
                            for (int n; (n = input.read(buffer)) != -1;) digest.update(buffer, 0, n);
                        }
                    }
                }
                StringBuilder hex = new StringBuilder(signers == null ? "apk:" : "cert:");
                for (byte b : digest.digest()) {
                    hex.append(Character.forDigit((b >>> 4) & 15, 16));
                    hex.append(Character.forDigit(b & 15, 16));
                }
                String identity = hex.toString(); mCodeIdentities.put(pkg, identity); return identity;
            } catch (Exception e) { throw new SecurityException("Cannot identify PendingIntent creator code", e); }
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
