/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.content.Intent;
import android.os.Build;
import android.os.Parcel;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Files;

/** Normal-format native compatibility only. No malformed or truncation inputs. */
public final class NotificationStoreNormalReference {
    private static final String PKG = "com.example.notificationnormal";
    private static final String POSTER = "com.example.originalposter";
    private static final String TAG = "normal-\u05d0-\ud83d\ude80-\ud800-fixture";
    private static final String KEY = "0|" + PKG + "|17|" + TAG + "|1000";
    private static final String SCHEMA = "gesher.notification.poster-and-toast.v1";
    private static int checks;
    private static void check(boolean value, String reason) {
        checks++;
        if (!value) throw new AssertionError(reason);
    }
    private static void providers() throws Exception {
        check(Build.VERSION.SDK_INT == 36 && "Dalvik".equals(System.getProperty("java.vm.name")),
                "Reference requires Android API36 ART");
        for (Class<?> type : new Class<?>[]{Parcel.class, Intent.class, Notification.class}) {
            check(Class.forName(type.getName(), false, null) == type, "Platform provider replaced: " + type);
            check(type.getClassLoader() != NotificationStore.class.getClassLoader(), "Fixture shadows platform provider: " + type);
        }
        check(NotificationStore.class.getClassLoader() == NotificationStoreNormalReference.class.getClassLoader(),
                "Production store must share the frozen fixture loader");
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(0x12345678); p.writeString(TAG);
            byte[] bytes = p.marshall();
            check(bytes[0] == 0x78 && bytes[1] == 0x56 && bytes[2] == 0x34 && bytes[3] == 0x12,
                    "Unexpected native scalar representation");
            p.setDataPosition(0);
            check(p.readInt() == 0x12345678 && TAG.equals(p.readString()) && p.dataAvail() == 0,
                    "Native Unicode round trip failed");
        } finally { p.recycle(); }
        System.out.println("NOTIFICATION_NORMAL_PROVIDER sdk=" + Build.VERSION.SDK_INT
                + " uid=" + android.os.Process.myUid() + " parcel=" + Parcel.class.getClassLoader()
                + " fixture=" + NotificationStore.class.getClassLoader());
    }
    private static File file() throws Exception {
        Method method = NotificationStore.class.getDeclaredMethod("file", String.class, String.class);
        method.setAccessible(true);
        return (File) method.invoke(null, PKG, KEY);
    }
    private static void intended(Parcel p, int kind) {
        p.writeInt(1); p.writeInt(kind); p.writeString(PKG); p.writeString("fixture-code-identity");
        p.writeInt(1000); p.writeInt(41); p.writeInt(0); p.writeInt(2);
        new Intent("fixture.notification.action").setPackage(PKG).writeToParcel(p, 0);
        new Intent("fixture.notification.next").setIdentifier("owned-\u05d0-\ud83d\ude80")
                .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK).writeToParcel(p, 0);
        p.writeStringArray(new String[]{null, "application/x-owned-fixture"});
    }
    private static NotificationStore.Stored historical(int version, boolean poster, boolean toast, int kind) throws Exception {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(version);
            if (version == 5) p.writeString(SCHEMA);
            p.writeString(KEY); p.writeString(PKG); p.writeString(null);
            p.writeString(TAG); p.writeInt(17); p.writeInt(0); p.writeString("normal-channel");
            if (poster) p.writeString(POSTER);
            if (toast) {
                p.writeString(NotificationManagerService.toastTag(TAG, 17));
                p.writeString(NotificationManagerService.legacyToastTag(TAG, 17));
            }
            intended(p, kind); p.writeInt(2); intended(p, kind); p.writeInt(0);
            File f = file(); f.getParentFile().mkdirs(); Files.write(f.toPath(), p.marshall());
        } finally { p.recycle(); }
        return NotificationStore.load(PKG, KEY);
    }
    private static void intents(NotificationStore.Stored s, int kind) {
        check(s != null && s.content != null && s.content.type == kind && s.content.requestCode == 41
                && s.content.uid == 1000 && PKG.equals(s.content.packageName)
                && "fixture-code-identity".equals(s.content.creatorIdentity), "Intended creator fields changed");
        check(s.content.intents.length == 2 && "fixture.notification.action".equals(s.content.intents[0].getAction())
                && PKG.equals(s.content.intents[0].getPackage())
                && "owned-\u05d0-\ud83d\ude80".equals(s.content.intents[1].getIdentifier())
                && s.content.intents[1].getFlags() == Intent.FLAG_ACTIVITY_NEW_TASK, "Framework Intent fields changed");
        check(s.content.resolvedTypes.length == 2 && s.content.resolvedTypes[0] == null
                && "application/x-owned-fixture".equals(s.content.resolvedTypes[1])
                && s.actions.length == 2 && s.actions[0] != null && s.actions[0].type == kind
                && s.actions[1] == null, "Resolved types or action state changed");
    }
    private static void currentFormat() throws Exception {
        byte[] bytes = Files.readAllBytes(file().toPath()); Parcel p = Parcel.obtain();
        try {
            p.unmarshall(bytes, 0, bytes.length); p.setDataPosition(0);
            check(p.dataSize() == bytes.length && p.readInt() == 5 && SCHEMA.equals(p.readString()),
                    "Actual writer did not produce the marked version5 format");
        } finally { p.recycle(); }
    }
    private static void normal(String phase) throws Exception {
        String current = NotificationManagerService.toastTag(TAG, 17);
        String legacy = NotificationManagerService.legacyToastTag(TAG, 17);
        if (phase.equals("write")) {
            for (int[] layout : new int[][]{{4, 1, 0}, {4, 0, 1}, {5, 1, 1}}) {
                for (int kind = 1; kind <= 5; kind++) {
                    NotificationStore.Stored s = historical(layout[0], layout[1] != 0, layout[2] != 0, kind);
                    check(s != null && KEY.equals(s.key) && TAG.equals(s.tag)
                            && (layout[1] != 0 ? POSTER : PKG).equals(s.opPkg), "Valid layout changed poster or Unicode identity");
                    check((layout[2] != 0 ? current : legacy).equals(s.windowsTag)
                            && (layout[2] != 0 ? legacy.equals(s.legacyWindowsTag) : s.legacyWindowsTag == null),
                            "Valid layout changed Windows metadata");
                    intents(s, kind);
                }
            }
            historical(4, true, false, 2);
            NotificationStore.save(null, KEY, PKG, TAG, 17, new Notification(), POSTER);
            NotificationStore.Stored migrated = NotificationStore.load(PKG, KEY);
            check(migrated != null && POSTER.equals(migrated.opPkg) && current.equals(migrated.windowsTag)
                    && legacy.equals(migrated.legacyWindowsTag), "Valid delegated repost lost its original identity");
            currentFormat();
            historical(4, false, true, 2);
            NotificationStore.legacyRemoved(PKG, KEY, legacy);
            migrated = NotificationStore.load(PKG, KEY);
            check(migrated != null && PKG.equals(migrated.opPkg) && migrated.legacyWindowsTag == null,
                    "Valid toast-v4 migration changed owner origin");
            intents(migrated, 2); currentFormat();
            historical(5, true, true, 2);
            NotificationStore.legacyRemoved(PKG, KEY, legacy);
            migrated = NotificationStore.load(PKG, KEY);
            check(migrated != null && POSTER.equals(migrated.opPkg) && migrated.legacyWindowsTag == null,
                    "Actual v5 rewrite changed original poster");
            intents(migrated, 2); currentFormat();
        } else {
            NotificationStore.Stored s = NotificationStore.load(PKG, KEY);
            check(s != null && POSTER.equals(s.opPkg) && TAG.equals(s.tag) && current.equals(s.windowsTag)
                    && s.legacyWindowsTag == null, "Actual v5 rewrite did not survive a separate native VM");
            intents(s, 2); currentFormat();
            NotificationStore.delete(PKG, KEY);
            check(NotificationStore.load(PKG, KEY) == null, "Owned retained record survived its own cancellation");
        }
    }
    public static void main(String[] args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
            System.err.println("NOTIFICATION_NORMAL_REFERENCE failed thread=" + thread.getName());
            failure.printStackTrace(System.err); System.err.flush(); System.exit(1);
        });
        if (args.length != 2 || !(args[0].equals("write") || args[0].equals("read"))
                || !args[1].matches("/data/local/tmp/gesher-notification-format-[0-9a-f]{12}")) {
            throw new IllegalArgumentException("Expected write/read and the owned fixture directory");
        }
        Device.fixtureDirectory = args[1];
        if (!new File(args[1]).isDirectory() && !new File(args[1]).mkdirs()) throw new AssertionError("Cannot create owned fixture directory");
        providers(); normal(args[0]);
        System.out.println("NOTIFICATION_NORMAL_REFERENCE passed phase=" + args[0] + " checks=" + checks
                + " scope=valid-native-Parcel-record-formats-only");
    }
}
