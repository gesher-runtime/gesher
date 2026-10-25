/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.Notification;
import android.app.PendingIntent;
import android.content.IIntentSender;
import android.content.Intent;
import android.os.Parcel;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * What a posted notification's toast needs once the system server that posted it is gone
 * (hwsvc2 track, ADR 0051): Windows keeps toasts in its notification center after the
 * Gesher device stops, and a click on one (Gesher's toast activator) must still send the
 * notification's content intent or its action's. Each posted notification's PendingIntents
 * (type, creator package, request code, flags, intents) are kept in
 * {@code $ANDROID_DATA/system/notifications/<package>/<hash of its key>.n}; the file goes
 * when it is cancelled, and the package's directory when it is uninstalled. A click whose
 * notification is no longer posted makes the PendingIntent again (as its creator would ask
 * for it) and sends it.
 */
final class NotificationStore {
    private static final int VERSION = 1;

    /** A notification as stored: its identity and its PendingIntents' records. */
    static final class Stored {
        String key;
        String packageName;
        String tag;
        int id;
        int flags;
        Intended content;
        Intended[] actions = new Intended[0];
    }

    /** A PendingIntent as PendingIntents.Record has it. */
    static final class Intended {
        int type;
        String packageName;
        int requestCode;
        int flags;
        Intent[] intents;
        String[] resolvedTypes;
    }

    private NotificationStore() {
    }

    static File dir(String pkg) {
        return new File(Device.dataDir(), "system/notifications/" + pkg);
    }

    private static File file(String pkg, String key) {
        return new File(dir(pkg), Integer.toHexString(key.hashCode()) + ".n");
    }

    private static Intended intended(ActivityManagerService am, PendingIntent pi) {
        if (pi == null || am == null) return null;
        PendingIntents.Record r = am.pendingIntents().record(pi.getTarget());
        if (r == null) return null;
        Intended i = new Intended();
        i.type = r.type;
        i.packageName = r.packageName;
        i.requestCode = r.requestCode;
        i.flags = r.flags;
        i.intents = r.intents;
        i.resolvedTypes = r.resolvedTypes;
        return i;
    }

    /** Keeps the notification's PendingIntents (for clicks after the device stopped). */
    static void save(ActivityManagerService am, String key, String pkg, String tag, int id,
            Notification n) {
        Stored s = new Stored();
        s.key = key;
        s.packageName = pkg;
        s.tag = tag;
        s.id = id;
        s.flags = n.flags;
        s.content = intended(am, n.contentIntent);
        int actions = n.actions != null ? n.actions.length : 0;
        s.actions = new Intended[actions];
        for (int i = 0; i < actions; i++) {
            s.actions[i] = n.actions[i] != null ? intended(am, n.actions[i].actionIntent) : null;
        }
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(VERSION);
            p.writeString(s.key);
            p.writeString(s.packageName);
            p.writeString(s.tag);
            p.writeInt(s.id);
            p.writeInt(s.flags);
            write(p, s.content);
            p.writeInt(s.actions.length);
            for (Intended i : s.actions) write(p, i);
            File f = file(pkg, key);
            f.getParentFile().mkdirs();
            try (OutputStream out = new FileOutputStream(f)) {
                out.write(p.marshall());
            }
        } catch (Throwable t) {
            SystemServer.log("notification: keeping " + key + " for later clicks failed: " + t);
        } finally {
            p.recycle();
        }
    }

    private static void write(Parcel p, Intended i) {
        if (i == null) {
            p.writeInt(0);
            return;
        }
        p.writeInt(1);
        p.writeInt(i.type);
        p.writeString(i.packageName);
        p.writeInt(i.requestCode);
        p.writeInt(i.flags);
        p.writeInt(i.intents.length);
        for (Intent intent : i.intents) intent.writeToParcel(p, 0);
        p.writeStringArray(i.resolvedTypes);
    }

    private static Intended read(Parcel p) {
        if (p.readInt() == 0) return null;
        Intended i = new Intended();
        i.type = p.readInt();
        i.packageName = p.readString();
        i.requestCode = p.readInt();
        i.flags = p.readInt();
        int n = p.readInt();
        i.intents = new Intent[n];
        for (int k = 0; k < n; k++) i.intents[k] = Intent.CREATOR.createFromParcel(p);
        i.resolvedTypes = p.createStringArray();
        return i;
    }

    /** The stored notification with {@code key} of {@code pkg}, or null. */
    static Stored load(String pkg, String key) {
        File f = file(pkg, key);
        if (!f.isFile()) return null;
        Parcel p = Parcel.obtain();
        try (InputStream in = new FileInputStream(f)) {
            byte[] data = in.readAllBytes();
            p.unmarshall(data, 0, data.length);
            p.setDataPosition(0);
            if (p.readInt() != VERSION) return null;
            Stored s = new Stored();
            s.key = p.readString();
            if (!key.equals(s.key)) return null;
            s.packageName = p.readString();
            s.tag = p.readString();
            s.id = p.readInt();
            s.flags = p.readInt();
            s.content = read(p);
            int actions = p.readInt();
            s.actions = new Intended[actions];
            for (int i = 0; i < actions; i++) s.actions[i] = read(p);
            return s;
        } catch (Throwable t) {
            SystemServer.log("notification: " + f + " not read: " + t);
            return null;
        } finally {
            p.recycle();
        }
    }

    static void delete(String pkg, String key) {
        file(pkg, key).delete();
    }

    /** Every stored notification of the package goes (an uninstall). */
    static void deleteAll(String pkg) {
        File d = dir(pkg);
        File[] files = d.listFiles();
        if (files != null) {
            for (File f : files) f.delete();
        }
        // Not File.delete: remove(3) removes no directory on Windows' C runtime.
        try {
            java.nio.file.Files.deleteIfExists(d.toPath());
        } catch (java.io.IOException e) {
            SystemServer.log("notification: " + d + " not removed: " + e);
        }
    }

    /** Sends a stored PendingIntent, made again as its creator would ask for it. */
    static int send(ActivityManagerService am, Intended i) {
        IIntentSender sender = am.pendingIntents().get(i.type, i.packageName, i.requestCode,
                i.intents, i.resolvedTypes,
                i.flags & ~(PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT));
        PendingIntents.Record r = am.pendingIntents().record(sender);
        return am.pendingIntents().send(r, null, null);
    }
}
