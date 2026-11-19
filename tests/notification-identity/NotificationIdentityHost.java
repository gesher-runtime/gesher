/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
import android.app.Notification;
import java.io.File;
import java.lang.reflect.*;
import java.nio.file.Files;
import sun.misc.Unsafe;

/** Real production persistence and identity methods; Parcel is an explicit host seam. */
public final class NotificationIdentityHost {
    static int checks;
    static final String PKG = "com.example.app", A = "0|com.example.app|7|Aa|10000",
            B = "0|com.example.app|7|BB|10000";
    static void check(boolean ok, String why) { ++checks; if (!ok) throw new AssertionError(why); }
    static Object call(Class<?> type, String name, Class<?>[] parameters, Object... args) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters); method.setAccessible(true);
        return method.invoke(null, args);
    }
    static File file(String key) throws Exception {
        return (File)call(NotificationStore.class, "file", new Class<?>[]{String.class, String.class}, PKG, key);
    }
    static File legacy(String key) { return new File(NotificationStore.dir(PKG), Integer.toHexString(key.hashCode()) + ".n"); }
    static Notification notification() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
        return (Notification)((Unsafe)field.get(null)).allocateInstance(Notification.class);
    }
    static void save(String key, String tag, int id) throws Exception {
        NotificationStore.save(null, key, PKG, tag, id, notification());
        check(file(key).isFile(), "save failed");
    }
    static void restored(String key, String tag, int id) {
        NotificationStore.Stored stored = NotificationStore.load(PKG, key);
        check(stored != null && key.equals(stored.key) && tag.equals(stored.tag) && stored.id == id,
                "notification lost or replaced by colliding identity");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[1]);
        check(!A.equals(B) && A.hashCode() == B.hashCode(), "invalid collision fixture");
        check(!file(A).equals(file(B)), "distinct notification keys share production storage path");
        check(!file("\ud800").equals(file("\ud801")), "unpaired tag code units collapsed");
        File imageA = (File)call(NotificationIcons.class, "largeFile", new Class<?>[]{String.class, String.class}, PKG, A);
        File imageB = (File)call(NotificationIcons.class, "largeFile", new Class<?>[]{String.class, String.class}, PKG, B);
        check(!imageA.equals(imageB), "distinct notification keys share image path");
        String longPackage = "com." + "a".repeat(200);
        File longImage = (File)call(NotificationIcons.class, "largeFile", new Class<?>[]{String.class, String.class}, longPackage, A);
        check(longImage.getName().length() <= 255, "valid long package exceeds Windows basename limit");
        check(!longImage.equals(imageA), "package identity missing from image path");
        if (args[0].equals("write")) { save(A, "Aa", 7); save(B, "BB", 7); }
        else {
            // Separate JVM invocation: persisted records survive process restart.
            restored(A, "Aa", 7); restored(B, "BB", 7);
            Files.copy(file(A).toPath(), legacy(A).toPath()); Files.delete(file(A).toPath());
            restored(A, "Aa", 7); // legacy read compatibility
            NotificationStore.delete(PKG, B);
            check(legacy(A).isFile(), "canceling colliding key deleted another legacy notification");
            restored(A, "Aa", 7);
            save(B, "BB", 7);
            check(legacy(A).isFile(), "saving colliding key deleted another legacy notification");
            save(A, "Aa", 7);
            check(!legacy(A).exists(), "same-key legacy record was not retired after successful update");
            restored(A, "Aa", 7); restored(B, "BB", 7);
            Files.copy(file(A).toPath(), legacy(A).toPath()); Files.delete(file(A).toPath());
            NotificationStore.delete(PKG, A);
            check(!legacy(A).exists() && NotificationStore.load(PKG, A) == null, "legacy cancellation did not remove its own record");
            restored(B, "BB", 7);
        }
        System.out.println("Notification identity host " + args[0] + " passed: " + checks + " checks");
        ToastIdentityDevice.contracts(args[0]);
        NotificationStoreFormatDevice.contracts(args[0]);
    }
}
