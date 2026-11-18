/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
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
    private static final int VERSION = 5;
    private static final String SCHEMA = "gesher.notification.poster-and-toast.v1";

    /** A notification as stored: its identity and its PendingIntents' records. */
    static final class Stored {
        String key;
        String packageName;
        String packageIdentity;
        String tag;
        int id;
        int flags;
        String channelId;
        String opPkg;
        String windowsTag;
        String legacyWindowsTag;
        Intended content;
        Intended[] actions = new Intended[0];
    }

    /** A PendingIntent as PendingIntents.Record has it. */
    static final class Intended {
        int type;
        String packageName;
        String creatorIdentity;
        int uid;
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
        return new File(dir(pkg), "sha256-" + NotificationIdentity.digest(key) + ".n");
    }

    private static File legacyFile(String pkg, String key) {
        return new File(dir(pkg), Integer.toHexString(key.hashCode()) + ".n");
    }

    private static Intended intended(ActivityManagerService am, PendingIntent pi) {
        if (pi == null || am == null) return null;
        PendingIntents.Record r = am.pendingIntents().record(pi.getTarget());
        if (r == null) return null;
        Intended i = new Intended();
        i.type = r.type;
        i.packageName = r.packageName;
        i.creatorIdentity = r.creatorIdentity;
        i.uid = r.uid;
        i.requestCode = r.requestCode;
        i.flags = r.flags;
        i.intents = r.intents;
        i.resolvedTypes = r.resolvedTypes;
        return i;
    }

    /** Keeps the notification's PendingIntents (for clicks after the device stopped). */
    static synchronized void save(ActivityManagerService am, String key, String pkg, String tag, int id,
            Notification n) {
        save(am, key, pkg, tag, id, n, pkg);
    }

    static synchronized void save(ActivityManagerService am, String key, String pkg, String tag, int id,
            Notification n, String opPkg) {
        Stored s = new Stored();
        s.key = key;
        s.packageName = pkg;
        s.packageIdentity = Services.sLast == null ? null
                : NotificationManagerService.codeIdentity(Services.sLast.pm.get(pkg));
        s.tag = tag;
        s.id = id;
        s.flags = n.flags;
        s.channelId = n.getChannelId() != null ? n.getChannelId() : "miscellaneous";
        s.opPkg = opPkg;
        s.windowsTag = NotificationManagerService.toastTag(tag, id);
        Stored previous = load(pkg, key);
        if (previous != null) {
            if (previous.legacyWindowsTag != null && NotificationManagerService.legacyToastTagMatches(previous.legacyWindowsTag, tag, id))
                s.legacyWindowsTag = previous.legacyWindowsTag;
            else if (NotificationManagerService.legacyToastTagMatches(previous.windowsTag, tag, id))
                s.legacyWindowsTag = previous.windowsTag;
        }
        s.content = intended(am, n.contentIntent);
        int actions = n.actions != null ? n.actions.length : 0;
        s.actions = new Intended[actions];
        for (int i = 0; i < actions; i++) {
            s.actions[i] = n.actions[i] != null ? intended(am, n.actions[i].actionIntent) : null;
        }
        writeStored(s);
    }

    private static void writeStored(Stored s) {
        String pkg = s.packageName, key = s.key;
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(VERSION);
            p.writeString(SCHEMA);
            p.writeString(s.key);
            p.writeString(s.packageName);
            p.writeString(s.packageIdentity);
            p.writeString(s.tag);
            p.writeInt(s.id);
            p.writeInt(s.flags);
            p.writeString(s.channelId);
            p.writeString(s.opPkg);
            p.writeString(s.windowsTag);
            p.writeString(s.legacyWindowsTag);
            write(p, s.content);
            p.writeInt(s.actions.length);
            for (Intended i : s.actions) write(p, i);
            File f = file(pkg, key);
            f.getParentFile().mkdirs();
            try (OutputStream out = new FileOutputStream(f)) {
                out.write(p.marshall());
            }
            deleteLegacy(pkg, key);
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
        p.writeString(i.creatorIdentity);
        p.writeInt(i.uid);
        p.writeInt(i.requestCode);
        p.writeInt(i.flags);
        p.writeInt(i.intents.length);
        for (Intent intent : i.intents) intent.writeToParcel(p, 0);
        p.writeStringArray(i.resolvedTypes);
    }

    private static int integer(Parcel p) {
        // Parcel's native scalar reader can return zero at EOF. A missing field
        // must not become a null PendingIntent or an empty action list.
        if (p.dataAvail() < 4) throw new IllegalArgumentException("Truncated notification record");
        return p.readInt();
    }

    private static String string(Parcel p) {
        int start = p.dataPosition(), length = integer(p);
        if (length < -1 || length > p.dataAvail() / 2) {
            throw new IllegalArgumentException("Invalid stored string length");
        }
        p.setDataPosition(start);
        String value = p.readString();
        if (length == -1 ? value != null : value == null || value.length() != length) {
            throw new IllegalArgumentException("Invalid stored string");
        }
        if (p.dataPosition() <= start || p.dataPosition() > p.dataSize()) {
            throw new IllegalArgumentException("Invalid stored string extent");
        }
        return value;
    }

    private static String[] strings(Parcel p, int expected) {
        int count = integer(p);
        if (count == -1) return null;
        if (count != expected || count > p.dataAvail() / 4) {
            throw new IllegalArgumentException("Invalid stored string array");
        }
        String[] values = new String[count];
        for (int i = 0; i < count; i++) values[i] = string(p);
        return values;
    }

    private static Intended read(Parcel p, int version) {
        int present = integer(p);
        if (present == 0) return null;
        if (present != 1) throw new IllegalArgumentException("Invalid stored intent presence");
        Intended i = new Intended();
        i.type = integer(p);
        switch (i.type) {
            case ActivityManager.INTENT_SENDER_BROADCAST:
            case ActivityManager.INTENT_SENDER_ACTIVITY:
            case ActivityManager.INTENT_SENDER_ACTIVITY_RESULT:
            case ActivityManager.INTENT_SENDER_SERVICE:
            case ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE:
                break;
            default: throw new IllegalArgumentException("Invalid stored intent sender kind");
        }
        i.packageName = string(p);
        if (version >= 2) {
            i.creatorIdentity = string(p);
            i.uid = integer(p);
        }
        i.requestCode = integer(p);
        i.flags = integer(p);
        int n = integer(p);
        if (n < 1 || n > 128) throw new IllegalArgumentException("Invalid stored intent count");
        i.intents = new Intent[n];
        for (int k = 0; k < n; k++) {
            int start = p.dataPosition();
            i.intents[k] = Intent.CREATOR.createFromParcel(p);
            if (p.dataPosition() <= start || p.dataPosition() > p.dataSize()) {
                throw new IllegalArgumentException("Invalid stored intent extent");
            }
        }
        i.resolvedTypes = strings(p, n);
        return i;
    }

    private static Stored tail(Parcel p, Stored header, int version, boolean poster, boolean toast) {
        Stored s = new Stored();
        s.key = header.key;
        s.packageName = header.packageName;
        s.packageIdentity = header.packageIdentity;
        s.tag = header.tag;
        s.id = header.id;
        s.flags = header.flags;
        s.channelId = header.channelId;
        s.opPkg = poster ? string(p) : s.packageName;
        if (s.opPkg == null || s.opPkg.isEmpty()) return null;
        s.windowsTag = toast ? string(p) : NotificationManagerService.legacyToastTag(s.tag, s.id);
        s.legacyWindowsTag = toast ? string(p) : null;
        if (toast && (!NotificationManagerService.toastTag(s.tag, s.id).equals(s.windowsTag)
                || s.legacyWindowsTag != null && !NotificationManagerService.legacyToastTagMatches(
                        s.legacyWindowsTag, s.tag, s.id))) return null;
        s.content = read(p, version);
        int actions = integer(p);
        if (actions < 0 || actions > 128) return null;
        s.actions = new Intended[actions];
        for (int i = 0; i < actions; i++) s.actions[i] = read(p, version);
        // Both historical v4 layouts share a prefix. Never accept a valid prefix
        // of the other format, a concatenated record or an absent final field.
        return p.dataAvail() == 0 ? s : null;
    }

    private static Stored historicalV4(Parcel p, Stored header, int position, boolean toast) {
        p.setDataPosition(position);
        try {
            return tail(p, header, 4, !toast, toast);
        } catch (RuntimeException malformed) {
            return null;
        }
    }

    /** The stored notification with {@code key} of {@code pkg}, or null. */
    static synchronized Stored load(String pkg, String key) {
        File current = file(pkg, key);
        // Existing Windows toasts keep their activation key after an upgrade.
        // The reader verifies the full key before accepting a legacy hash file.
        return load(pkg, key, current.isFile() ? current : legacyFile(pkg, key));
    }

    private static Stored load(String pkg, String key, File f) {
        if (!f.isFile()) return null;
        Parcel p = Parcel.obtain();
        try (InputStream in = new FileInputStream(f)) {
            byte[] data = in.readAllBytes();
            p.unmarshall(data, 0, data.length);
            // JNI unmarshalling writes through Parcel::writeInplace, which can
            // pad a truncated final word with zero bytes. Keep the actual file
            // extent authoritative before those bytes can become valid fields.
            if (p.dataSize() != data.length) return null;
            p.setDataPosition(0);
            int version = integer(p);
            if (version != VERSION && version != 4 && version != 3 && version != 2
                    && !(version == 1 && !GmsSignatureSpoofing.isEnabled())) return null;
            if (version == VERSION && !SCHEMA.equals(string(p))) return null;
            Stored s = new Stored();
            s.key = string(p);
            if (s.key == null || key != null && !key.equals(s.key)) return null;
            s.packageName = string(p);
            if (!pkg.equals(s.packageName)) return null;
            if (version >= 2) s.packageIdentity = string(p);
            if (GmsSignatureSpoofing.isEnabled() && (s.packageIdentity == null || Services.sLast == null
                    || !s.packageIdentity.equals(NotificationManagerService.codeIdentity(
                            Services.sLast.pm.get(s.packageName))))) return null;
            s.tag = string(p);
            s.id = integer(p);
            s.flags = integer(p);
            if (version >= 3) s.channelId = string(p);
            if (version == 4) {
                int position = p.dataPosition();
                Stored delegated = historicalV4(p, s, position, false);
                Stored identity = historicalV4(p, s, position, true);
                // Separate published branches used v4 for poster attribution and
                // toast migration. Only a unique complete interpretation can
                // establish whether the poster was stored or was owner-only.
                return (delegated == null) == (identity == null) ? null
                        : delegated != null ? delegated : identity;
            }
            return tail(p, s, version, version == VERSION, version == VERSION);
        } catch (Throwable t) {
            SystemServer.log("notification: " + f + " not read: " + t);
            return null;
        } finally {
            p.recycle();
        }
    }

    static synchronized void delete(String pkg, String key) {
        file(pkg, key).delete();
        deleteLegacy(pkg, key);
    }

    static synchronized void legacyRemoved(String pkg, String key, String legacyTag) {
        Stored s = load(pkg, key);
        if (s != null && legacyTag.equals(s.legacyWindowsTag)) {
            s.legacyWindowsTag = null;
            writeStored(s);
        }
    }

    private static void deleteLegacy(String pkg, String key) {
        File legacy = legacyFile(pkg, key);
        if (load(pkg, key, legacy) != null) legacy.delete();
    }

    /** Retained Windows toasts can outlive the system server's in-memory records. */
    static synchronized java.util.List<Stored> deleteChannels(String pkg, java.util.Set<String> channelIds) {
        java.util.List<Stored> removed = new java.util.ArrayList<>();
        File[] files = dir(pkg).listFiles((directory, name) -> name.endsWith(".n"));
        if (files == null) return removed;
        for (File f : files) {
            Stored stored = load(pkg, null, f);
            if (stored != null) {
                String ch = stored.channelId != null ? stored.channelId : "miscellaneous";
                if (channelIds.contains(ch)) {
                    f.delete();
                    removed.add(stored);
                }
            }
        }
        return removed;
    }

    /** Every stored notification of the package goes (an uninstall). */
    static synchronized void deleteAll(String pkg) {
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
        if (GmsSignatureSpoofing.isEnabled()) {
            PackageManagerService pm = Services.sLast == null ? null : Services.sLast.pm;
            ApkPackage creator = pm == null ? null : pm.get(i.packageName);
            boolean systemCreator = "android".equals(i.packageName)
                    && i.uid == android.os.Process.SYSTEM_UID && "android-system".equals(i.creatorIdentity);
            if (!systemCreator && (creator == null || creator.uid() != i.uid
                    || i.creatorIdentity == null || !i.creatorIdentity.equals(
                            NotificationManagerService.codeIdentity(creator)))) return -1;
        }
        IIntentSender sender = am.pendingIntents().get(i.type, i.packageName, i.requestCode,
                i.intents, i.resolvedTypes,
                i.flags & ~(PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT));
        PendingIntents.Record r = am.pendingIntents().record(sender);
        if (GmsSignatureSpoofing.isEnabled() && (r == null || r.uid != i.uid
                || i.creatorIdentity == null || !i.creatorIdentity.equals(r.creatorIdentity))) return -1;
        return am.pendingIntents().send(r, null, null);
    }
}
