/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.os.Parcel;
import android.os.UserHandle;
import android.service.notification.StatusBarNotification;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

/** Real record/cancellation/retirement paths; Windows history/removal is an explicit seam. */
public final class ToastIdentityDevice {
    static int checks;
    static final String PKG = "com.example.toastidentity", A = "Aa".repeat(40), B = "BB".repeat(40);
    static String key(String tag) { return "0|" + PKG + "|7|" + tag + "|1000"; }
    static void check(boolean ok, String why) { ++checks; if (!ok) throw new AssertionError(why); }
    static File file(String key) throws Exception {
        Method method = NotificationStore.class.getDeclaredMethod("file", String.class, String.class);
        method.setAccessible(true); return (File)method.invoke(null, PKG, key);
    }
    static void legacy(String tag) throws Exception {
        legacyRecord(key(tag), tag, 7);
    }
    static void legacyRecord(String key, String tag, int id) throws Exception {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(3); p.writeString(key); p.writeString(PKG); p.writeString(null);
            p.writeString(tag); p.writeInt(id); p.writeInt(0); p.writeString("identity");
            p.writeInt(0); p.writeInt(0); // No content/action PendingIntents.
            File file = file(key); file.getParentFile().mkdirs(); Files.write(file.toPath(), p.marshall());
        } finally { p.recycle(); }
    }
    static NotificationStore.Stored stored(String tag) { return NotificationStore.load(PKG, key(tag)); }
    static void save(String tag) { NotificationStore.save(null, key(tag), PKG, tag, 7, new Notification()); }
    static final class History {
        final Map<String,String> owners = new HashMap<>();
        boolean fail;
        int remove(String tag, String expected) {
            if (fail) return 0x80004005;
            if (expected != null && owners.containsKey(tag) && !Objects.equals(owners.get(tag), expected)) return 1;
            owners.remove(tag); return 0;
        }
    }
    static NotificationManagerService service(History history) throws Exception {
        NotificationManagerService service = new NotificationManagerService(null, null, (n,u) -> 0);
        Field field = NotificationManagerService.class.getDeclaredField("mTestToastRemoval"); field.setAccessible(true);
        field.set(service, (java.util.function.ToIntBiFunction<String,String>)history::remove);
        return service;
    }
    static void retire(NotificationManagerService service, String tag) throws Exception {
        Method method = NotificationManagerService.class.getDeclaredMethod("retireLegacyToast", String.class, String.class);
        method.setAccessible(true); method.invoke(service, PKG, key(tag));
    }
    public static void contracts(String phase) throws Exception {
        String old = NotificationManagerService.legacyToastTag(A, 7);
        String currentA = NotificationManagerService.toastTag(A, 7), currentB = NotificationManagerService.toastTag(B, 7);
        check(A.hashCode() == B.hashCode() && old.equals(NotificationManagerService.legacyToastTag(B, 7)), "Invalid old collision fixture");
        check(!currentA.equals(currentB) && currentA.length() == 64 && currentB.length() == 64, "Current toast identities collide/exceed limit");
        String shortPeer = Integer.toHexString(A.hashCode());
        check(old.equals(NotificationManagerService.legacyToastTag(shortPeer, 7))
                && !old.equals(NotificationManagerService.toastTag(shortPeer, 7)), "Current short tag aliases legacy namespace");
        History history = new History();
        NotificationManagerService service = service(history);
        String nullKey = new StatusBarNotification(PKG, PKG, 8, null, 1000, 0, new Notification(), UserHandle.SYSTEM, null, 0).getKey();
        String textKey = new StatusBarNotification(PKG, PKG, 8, "null", 1000, 0, new Notification(), UserHandle.SYSTEM, null, 0).getKey();
        check(nullKey.equals(textKey) && NotificationManagerService.toastTag(null, 8).equals(NotificationManagerService.toastTag("null", 8)),
                "Toast mapping changed the platform's existing null-tag key equivalence");
        if (phase.equals("write")) {
            legacyRecord(nullKey, null, 8);
            NotificationStore.save(null, textKey, PKG, "null", 8, new Notification());
            NotificationStore.Stored nullUpdate = NotificationStore.load(PKG, textKey);
            check(nullUpdate != null && "8".equals(nullUpdate.legacyWindowsTag), "Equivalent null-tag update lost its legacy identity");
            NotificationStore.save(null, nullKey, PKG, null, 8, new Notification());
            check(NotificationStore.load(PKG, nullKey) != null, "Reverse null-tag update corrupted metadata");
            legacy(A); legacy(B); // Both v3 records can exist after the filename repair.
            check(stored(A).windowsTag.equals(old) && stored(B).windowsTag.equals(old), "Legacy mapping was rewritten on read");
            save(A); save(B);
            check(stored(A).windowsTag.equals(currentA) && stored(A).legacyWindowsTag.equals(old), "Upgrade lost cleanup alias");
            history.owners.put(currentA, key(A)); history.owners.put(currentB, key(B)); history.owners.put(old, key(B));
            service.cancel(PKG, A, 7);
            check(!history.owners.containsKey(currentA) && history.owners.get(old).equals(key(B))
                    && history.owners.get(currentB).equals(key(B)), "Cancel removed a colliding current/legacy peer");
            check(stored(A) == null && stored(B) != null, "Cancel damaged peer persistence");
            // A successful repost retires only its own old UI identity. Errors and
            // owner mismatches retain metadata for cancellation after a restart.
            legacy(A); save(A);
            history.owners.put(old, key(A)); history.owners.put(currentA, key(A));
            history.fail = true; retire(service, A);
            check(stored(A).legacyWindowsTag.equals(old), "History failure fabricated migration success");
            history.fail = false; history.owners.put(old, key(B)); retire(service, A);
            check(stored(A).legacyWindowsTag.equals(old) && history.owners.get(old).equals(key(B)), "Owner mismatch removed a peer");
            history.owners.put(old, key(A)); retire(service, A);
            check(stored(A).legacyWindowsTag == null && !history.owners.containsKey(old), "Own legacy identity not retired");
            // Keep B's unretired alias on disk for the next VM, modeling failed cleanup.
        } else {
            NotificationStore.Stored nullUpdate = NotificationStore.load(PKG, textKey);
            check(nullUpdate != null && "8".equals(nullUpdate.legacyWindowsTag), "Null-tag update did not survive restart");
            history.owners.put("8", nullKey); history.owners.put("8|null", nullKey);
            history.owners.put(nullUpdate.windowsTag, nullKey);
            service.cancel(PKG, "null", 8);
            check(!history.owners.containsKey("8") && !history.owners.containsKey("8|null") && !history.owners.containsKey(nullUpdate.windowsTag)
                    && NotificationStore.load(PKG, nullKey) == null, "Null-equivalent restart cancellation missed an identity");
            check(stored(B).windowsTag.equals(currentB) && stored(B).legacyWindowsTag.equals(old), "Restart lost v4 identity/alias");
            history.owners.put(currentB, key(B)); history.owners.put(old, key(B)); history.owners.put(currentA, key(A));
            service.cancel(PKG, B, 7);
            check(!history.owners.containsKey(currentB) && !history.owners.containsKey(old)
                    && history.owners.get(currentA).equals(key(A)), "Restart cancellation missed old/current identity or removed peer");
            check(stored(B) == null && stored(A) != null, "Restart cancellation damaged another record");
            service.cancel(PKG, A, 7);
        }
        System.out.println("ToastIdentityDevice " + phase + " passed: " + checks + " checks");
    }
}
