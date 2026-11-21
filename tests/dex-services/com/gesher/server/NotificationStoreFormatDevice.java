/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.content.Intent;
import android.os.Parcel;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;

/** Published record layouts, decoded by the actual store. No old reader is executed. */
public final class NotificationStoreFormatDevice {
    private static final String PKG = "com.example.notificationformat";
    private static final String POSTER = "com.example.originalposter";
    private static final String TAG = "owned-\u05d0-\ud83d\ude80-\ud800-fixture";
    private static final String KEY = "0|" + PKG + "|17|" + TAG + "|1000";
    private static final String SCHEMA = "gesher.notification.poster-and-toast.v1";
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static File file() throws Exception {
        return file(KEY);
    }

    private static File file(String key) throws Exception {
        Method method = NotificationStore.class.getDeclaredMethod("file", String.class, String.class);
        method.setAccessible(true);
        return (File) method.invoke(null, PKG, key);
    }

    private static byte[] record(int version, boolean poster, boolean toast, String opPkg,
            String current, String legacy, String schema, int presence, int actions) {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(version);
            if (version == 5) p.writeString(schema);
            p.writeString(KEY); p.writeString(PKG);
            if (version >= 2) p.writeString(null);
            p.writeString(TAG); p.writeInt(17); p.writeInt(0);
            if (version >= 3) p.writeString("format-channel");
            if (poster) p.writeString(opPkg);
            if (toast) { p.writeString(current); p.writeString(legacy); }
            p.writeInt(presence); p.writeInt(actions);
            for (int i = 0; i < actions && i < 128; i++) p.writeInt(0);
            return p.marshall();
        } finally { p.recycle(); }
    }

    private static byte[] valid(int version, boolean poster, boolean toast) {
        return record(version, poster, toast, POSTER,
                NotificationManagerService.toastTag(TAG, 17),
                NotificationManagerService.legacyToastTag(TAG, 17), SCHEMA, 0, 2);
    }

    private static void intended(Parcel p, int version, int kind) {
        intended(p, version, kind, 2);
    }

    private static void intended(Parcel p, int version, int kind, int resolvedCount) {
        p.writeInt(1); p.writeInt(kind); p.writeString(PKG);
        if (version >= 2) { p.writeString("fixture-code-identity"); p.writeInt(1000); }
        p.writeInt(41); p.writeInt(0); p.writeInt(2);
        new Intent("fixture.notification.action").setPackage(PKG).writeToParcel(p, 0);
        new Intent("fixture.notification.next").setIdentifier("owned-\u05d0-\ud83d\ude80")
                .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK).writeToParcel(p, 0);
        String[] types = new String[resolvedCount];
        if (resolvedCount > 1) types[1] = "application/x-owned-fixture";
        p.writeStringArray(types);
    }

    private static byte[] withIntents(int version, boolean poster, boolean toast, int kind) {
        return withIntents(version, poster, toast, kind, 2);
    }

    private static byte[] withIntents(int version, boolean poster, boolean toast, int kind, int resolvedCount) {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(version);
            if (version == 5) p.writeString(SCHEMA);
            p.writeString(KEY); p.writeString(PKG);
            if (version >= 2) p.writeString(null);
            p.writeString(TAG); p.writeInt(17); p.writeInt(0);
            if (version >= 3) p.writeString("format-channel");
            if (poster) p.writeString(POSTER);
            if (toast) {
                p.writeString(NotificationManagerService.toastTag(TAG, 17));
                p.writeString(NotificationManagerService.legacyToastTag(TAG, 17));
            }
            intended(p, version, kind, resolvedCount); p.writeInt(2); intended(p, version, kind); p.writeInt(0);
            return p.marshall();
        } finally { p.recycle(); }
    }

    private static void checkIntents(NotificationStore.Stored s, int version, int kind) {
        check(s != null && s.content != null && s.content.type == kind && s.content.requestCode == 41
                && PKG.equals(s.content.packageName) && s.actions.length == 2 && s.actions[0] != null
                && s.actions[0].type == kind && s.actions[1] == null, "Stored PendingIntent/action layout changed");
        check(s.content.intents.length == 2 && "fixture.notification.action".equals(s.content.intents[0].getAction())
                && PKG.equals(s.content.intents[0].getPackage())
                && "owned-\u05d0-\ud83d\ude80".equals(s.content.intents[1].getIdentifier())
                && s.content.intents[1].getFlags() == Intent.FLAG_ACTIVITY_NEW_TASK
                && s.content.resolvedTypes.length == 2 && s.content.resolvedTypes[0] == null
                && "application/x-owned-fixture".equals(s.content.resolvedTypes[1]),
                "Stored framework Intent or resolved type changed");
        check(version == 1 ? s.content.creatorIdentity == null && s.content.uid == 0
                : "fixture-code-identity".equals(s.content.creatorIdentity) && s.content.uid == 1000,
                "Stored PendingIntent creator identity changed");
    }

    private static void oneDigitAliases() throws Exception {
        // A one-character String16 alias starts with integer1, the same prefix
        // as a present Intended. Its character is not a valid sender kind.
        for (int id = 0; id < 10; id++) {
            String key = "0|" + PKG + "|" + id + "|null|1000";
            Parcel p = Parcel.obtain();
            try {
                p.writeInt(4); p.writeString(key); p.writeString(PKG); p.writeString(null);
                p.writeString(null); p.writeInt(id); p.writeInt(0); p.writeString("format-channel");
                p.writeString(NotificationManagerService.toastTag(null, id)); p.writeString(Integer.toString(id));
                intended(p, 4, 2); p.writeInt(0);
                File f = file(key); f.getParentFile().mkdirs(); Files.write(f.toPath(), p.marshall());
                NotificationStore.Stored s = NotificationStore.load(PKG, key);
                check(s != null && PKG.equals(s.opPkg) && Integer.toString(id).equals(s.legacyWindowsTag)
                        && s.content != null && s.content.type == 2,
                        "Single-digit toast alias was interpreted as a delegated PendingIntent");
                NotificationStore.delete(PKG, key);
            } finally { p.recycle(); }
        }
    }

    private static NotificationStore.Stored install(byte[] bytes) throws Exception {
        File f = file(); f.getParentFile().mkdirs(); Files.write(f.toPath(), bytes);
        return NotificationStore.load(PKG, KEY);
    }

    private static void invalid(byte[] bytes, String message) throws Exception {
        check(install(bytes) == null, message);
        check(Arrays.equals(bytes, Files.readAllBytes(file().toPath())), "Rejected record was rewritten");
    }

    private static void currentFormat() throws Exception {
        byte[] bytes = Files.readAllBytes(file().toPath()); Parcel p = Parcel.obtain();
        try {
            p.unmarshall(bytes, 0, bytes.length); p.setDataPosition(0);
            check(p.readInt() == 5 && SCHEMA.equals(p.readString()), "Writer omitted version5 schema marker");
        } finally { p.recycle(); }
    }

    public static void contracts(String phase) throws Exception {
        checks = 0;
        String current = NotificationManagerService.toastTag(TAG, 17);
        String legacy = NotificationManagerService.legacyToastTag(TAG, 17);
        if (phase.equals("read")) {
            NotificationStore.Stored s = NotificationStore.load(PKG, KEY);
            check(s != null && POSTER.equals(s.opPkg) && current.equals(s.windowsTag)
                    && legacy.equals(s.legacyWindowsTag), "Combined attribution/cleanup state did not survive restart");
            checkIntents(s, 5, 2);
            NotificationStore.legacyRemoved(PKG, KEY, legacy);
            s = NotificationStore.load(PKG, KEY);
            check(s != null && POSTER.equals(s.opPkg) && s.legacyWindowsTag == null,
                    "Alias retirement rewrote the original poster");
            checkIntents(s, 5, 2);
            currentFormat();
            NotificationStore.delete(PKG, KEY);
            check(NotificationStore.load(PKG, KEY) == null, "Combined state survived its own cancellation");
        } else {
            for (int version = 1; version <= 3; version++) {
                NotificationStore.Stored s = install(valid(version, false, false));
                check(s != null && TAG.equals(s.tag) && PKG.equals(s.opPkg) && legacy.equals(s.windowsTag)
                        && s.legacyWindowsTag == null && s.actions.length == 2,
                        "Predelegation version " + version + " changed owner or Windows identity");
            }
            NotificationStore.Stored delegated = install(valid(4, true, false));
            check(delegated != null && POSTER.equals(delegated.opPkg) && legacy.equals(delegated.windowsTag)
                    && delegated.legacyWindowsTag == null && delegated.actions.length == 2,
                    "Published delegation-v4 layout was not preserved");
            NotificationStore.Stored identity = install(valid(4, false, true));
            check(identity != null && PKG.equals(identity.opPkg) && current.equals(identity.windowsTag)
                    && legacy.equals(identity.legacyWindowsTag) && identity.actions.length == 2,
                    "Published toast-v4 layout was interpreted as delegated posting");
            identity = install(record(4, false, true, null, current, null, null, 0, 0));
            check(identity != null && PKG.equals(identity.opPkg) && identity.legacyWindowsTag == null,
                    "Toast-v4 without a cleanup alias did not migrate");
            NotificationStore.Stored combined = install(valid(5, true, true));
            check(combined != null && POSTER.equals(combined.opPkg) && current.equals(combined.windowsTag)
                    && legacy.equals(combined.legacyWindowsTag), "Combined record lost a stored identity");
            for (int[] layout : new int[][]{{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 1, 0}, {4, 0, 1}, {5, 1, 1}}) {
                for (int kind = 1; kind <= 5; kind++) {
                    checkIntents(install(withIntents(layout[0], layout[1] != 0, layout[2] != 0, kind)), layout[0], kind);
                }
            }
            oneDigitAliases();
            invalid(new byte[0], "Empty record was accepted");
            invalid(new byte[3], "Truncated version field was accepted");
            for (int length : new int[]{-2, Integer.MAX_VALUE}) {
                Parcel prefix = Parcel.obtain();
                try {
                    prefix.writeInt(5); prefix.writeInt(length);
                    invalid(prefix.marshall(), "Invalid schema string extent was accepted");
                } finally { prefix.recycle(); }
            }
            for (int version : new int[]{4, 5}) {
                invalid(record(version, true, version == 5, null, current, legacy, SCHEMA, 0, 0),
                        "Missing poster was inferred from owner");
                invalid(record(version, true, version == 5, "", current, legacy, SCHEMA, 0, 0),
                        "Empty poster was inferred from owner");
            }
            invalid(record(4, true, true, POSTER, current, legacy, SCHEMA, 0, 0),
                    "Unmarked hybrid-v4 layout was accepted");
            invalid(record(4, false, true, null, "unverified-current", legacy, null, 0, 0),
                    "Invalid Windows identity was interpreted as poster attribution");
            invalid(record(4, false, true, null, current, "another-notification", null, 0, 0),
                    "Unverified toast-v4 cleanup alias was accepted");
            invalid(record(5, true, true, POSTER, current, legacy, "unrecognized-schema", 0, 0),
                    "Unrecognized version5 schema was accepted");
            invalid(record(6, true, true, POSTER, current, legacy, SCHEMA, 0, 0),
                    "Unknown record version was accepted");
            invalid(record(5, true, true, POSTER, current, legacy, SCHEMA, 2, 0),
                    "Invalid PendingIntent presence marker was accepted");
            invalid(record(5, true, true, POSTER, current, legacy, SCHEMA, 1, 0),
                    "Invalid PendingIntent sender kind was accepted");
            invalid(record(5, true, true, POSTER, current, legacy, SCHEMA, 0, -1),
                    "Negative action count was accepted");
            int layoutIndex = 0;
            for (byte[] complete : new byte[][]{valid(1, false, false), valid(2, false, false),
                    valid(3, false, false), valid(4, true, false), valid(4, false, true), valid(5, true, true)}) {
                for (int missing : new int[]{1, 4, 8, 12, 16}) {
                    invalid(Arrays.copyOf(complete, complete.length - missing),
                            "Truncated record synthesized missing fields: layout=" + layoutIndex
                                    + " fullBytes=" + complete.length + " missingBytes=" + missing);
                }
                invalid(Arrays.copyOf(complete, complete.length + 4), "Trailing bytes were ignored");
                invalid(Arrays.copyOf(complete, complete.length + 1), "Unaligned trailing byte was ignored");
                byte[] joined = Arrays.copyOf(complete, complete.length * 2);
                System.arraycopy(complete, 0, joined, complete.length, complete.length);
                invalid(joined, "Concatenated records were accepted");
                layoutIndex++;
            }
            byte[] fullIntents = withIntents(5, true, true, 2);
            invalid(Arrays.copyOf(fullIntents, fullIntents.length - 4), "Truncated final PendingIntent action was accepted");
            // Cut throughout real framework Intent fields and resolved-type data,
            // not only the final null action marker. The same corpus runs on ART.
            for (int length = 4; length < fullIntents.length; length += Math.max(4, (fullIntents.length / 24) & ~3)) {
                invalid(Arrays.copyOf(fullIntents, length), "Truncated framework Intent/type payload was accepted: bytes="
                        + length + " fullBytes=" + fullIntents.length);
            }
            for (int count : new int[]{0, 1, 3, 129}) {
                invalid(withIntents(5, true, true, 2, count), "Resolved-type count differs from stored intent count");
            }
            // Update a published delegation record. Save must preserve its real
            // old Windows identity while recording the new digest and poster.
            install(valid(4, true, false));
            NotificationStore.save(null, KEY, PKG, TAG, 17, new Notification(), POSTER);
            combined = NotificationStore.load(PKG, KEY);
            check(combined != null && POSTER.equals(combined.opPkg) && current.equals(combined.windowsTag)
                    && legacy.equals(combined.legacyWindowsTag), "Delegated repost lost old Windows identity");
            currentFormat();
            install(withIntents(5, true, true, 2)); // Preserve full content/actions for the separate restart VM.
        }
        System.out.println("NotificationStoreFormatDevice " + phase + " passed: " + checks + " checks");
    }
}
