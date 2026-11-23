/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.content.Intent;
import android.os.Build;
import android.os.Parcel;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Files;

/** Native Android Parcel qualification only; all other service boundaries are explicit fixtures. */
public final class NotificationStoreReference {
    private static int providerChecks, identityChecks;
    private static void provider(boolean value, String reason) {
        providerChecks++;
        if (!value) throw new AssertionError(reason);
    }
    private static void identity(boolean value, String reason) {
        identityChecks++;
        if (!value) throw new AssertionError(reason);
    }
    private static void providers() throws Exception {
        provider(Build.VERSION.SDK_INT == 36, "Native reference requires Android API36");
        provider("Dalvik".equals(System.getProperty("java.vm.name")), "Reference is not running on Android ART");
        for (Class<?> type : new Class<?>[]{Parcel.class, Intent.class, Notification.class}) {
            provider(Class.forName(type.getName(), false, null) == type, "Platform provider was replaced: " + type);
            provider(type.getClassLoader() != NotificationStore.class.getClassLoader(), "Fixture shadows platform provider: " + type);
        }
        provider(NotificationStore.class.getClassLoader() == NotificationStoreFormatDevice.class.getClassLoader(),
                "Production parser and corpus must come from the same frozen DEX");
        Parcel p = Parcel.obtain();
        try {
            String value = "A\u05d0\ud83d\ude80\ud800";
            p.writeInt(0x12345678); p.writeString(value);
            byte[] bytes = p.marshall();
            provider(bytes.length >= 4 && bytes[0] == 0x78 && bytes[1] == 0x56
                    && bytes[2] == 0x34 && bytes[3] == 0x12, "Unexpected native Parcel integer representation");
            p.setDataPosition(0);
            provider(p.readInt() == 0x12345678 && value.equals(p.readString()) && p.dataAvail() == 0,
                    "Native Unicode Parcel round trip or byte extent failed");
        } finally { p.recycle(); }
        Parcel padded = Parcel.obtain();
        try {
            padded.unmarshall(new byte[3], 0, 3);
            provider(padded.dataSize() == 4, "Unexpected native unmarshalling extent for an incomplete word");
            System.out.println("NOTIFICATION_FORMAT_EXTENT inputBytes=3 nativeDataSize=" + padded.dataSize());
        } finally { padded.recycle(); }
        System.out.println("NOTIFICATION_FORMAT_PROVIDER sdk=" + Build.VERSION.SDK_INT
                + " uid=" + android.os.Process.myUid() + " vm=" + System.getProperty("java.vm.name")
                + " parcel=" + Parcel.class.getClassLoader() + " fixture=" + NotificationStore.class.getClassLoader());
    }

    private static void identityGuards() throws Exception {
        String pkg = "com.example.formatidentity", key = "0|" + pkg + "|9|null|10000";
        ApkPackage owner = new ApkPackage(); owner.name = pkg; owner.fixtureCodeIdentity = "fixture-code-A";
        PackageManagerService pm = new PackageManagerService(); pm.owner = owner;
        Services.Created services = new Services.Created(); services.pm = pm; Services.sLast = services;
        GmsSignatureSpoofing.protectedIdentity = true;
        try {
            NotificationStore.save(null, key, pkg, null, 9, new Notification(), pkg);
            identity(NotificationStore.load(pkg, key) != null, "Matching supplied owner identity failed");
            owner.fixtureCodeIdentity = "fixture-code-B";
            identity(NotificationStore.load(pkg, key) == null, "Changed supplied owner identity was ignored");
            owner.fixtureCodeIdentity = null;
            identity(NotificationStore.load(pkg, key) == null, "Missing supplied owner identity was accepted");
            owner.fixtureCodeIdentity = "fixture-code-A"; pm.owner = null;
            identity(NotificationStore.load(pkg, key) == null, "Missing supplied owner package was accepted");
            pm.owner = owner; Services.sLast = null;
            identity(NotificationStore.load(pkg, key) == null, "Missing identity service was accepted");
            Services.sLast = services;
            identity(NotificationStore.load(pkg, key) != null, "Restoring supplied identity corrupted retained record");
            NotificationStore.delete(pkg, key);
            Parcel p = Parcel.obtain();
            try {
                p.writeInt(1); p.writeString(key); p.writeString(pkg); p.writeString(null);
                p.writeInt(9); p.writeInt(0); p.writeInt(0); p.writeInt(0);
                Method file = NotificationStore.class.getDeclaredMethod("file", String.class, String.class);
                file.setAccessible(true); File destination = (File) file.invoke(null, pkg, key);
                Files.write(destination.toPath(), p.marshall());
            } finally { p.recycle(); }
            identity(NotificationStore.load(pkg, key) == null, "Identity-less v1 accepted in protected mode");
            GmsSignatureSpoofing.protectedIdentity = false;
            NotificationStore.Stored historical = NotificationStore.load(pkg, key);
            identity(historical != null && pkg.equals(historical.opPkg), "Legacy-mode v1 lost genuine owner origin");
            NotificationStore.delete(pkg, key);
        } finally { GmsSignatureSpoofing.protectedIdentity = false; Services.sLast = null; }
    }

    public static void main(String[] args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
            System.err.println("NOTIFICATION_STORE_REFERENCE failed thread=" + thread.getName());
            failure.printStackTrace(System.err); System.err.flush();
            System.exit(1);
        });
        if (args.length != 2 || !(args[0].equals("write") || args[0].equals("read"))
                || !args[1].matches("/data/local/tmp/gesher-notification-format-[0-9a-f]{12}")) {
            throw new IllegalArgumentException("Expected write/read and the owned format fixture directory");
        }
        Device.fixtureDirectory = args[1];
        if (!new File(args[1]).isDirectory() && !new File(args[1]).mkdirs()) throw new AssertionError("Cannot create owned directory");
        providers();
        NotificationStoreFormatDevice.contracts(args[0]);
        if (args[0].equals("read")) identityGuards();
        System.out.println("NOTIFICATION_STORE_REFERENCE passed phase=" + args[0]
                + " providerChecks=" + providerChecks + " suppliedIdentityChecks=" + identityChecks
                + " scope=native-Parcel-record-format-only");
    }
}
