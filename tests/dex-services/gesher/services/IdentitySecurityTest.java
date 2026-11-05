/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.IActivityManager;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.ServiceManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Properties;

/**
 * Exercises the production UID registry and process-authentication boundary.
 * Default: args[0] is the signing fixtures directory, using a fresh test data root.
 * Cross-VM persistence: run "uid-write DATA_ROOT", then "uid-read DATA_ROOT".
 * "remote-denied PACKAGE PERMISSION" runs with an unattached non-system UID
 * against an existing test device, checking real Binder rejection paths.
 * No APK application code, external Binder endpoint or network connection is used.
 */
public final class IdentitySecurityTest {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }

    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        Method result = type.getDeclaredMethod(name, types); result.setAccessible(true); return result;
    }

    private static Object construct(Class<?> type, Class<?>[] types, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(types);
        constructor.setAccessible(true); return constructor.newInstance(values);
    }

    private interface Operation { void run() throws Exception; }

    private static void denied(Operation operation, String messagePart) throws Exception {
        try { operation.run(); }
        catch (SecurityException failure) {
            require(failure.getMessage().contains(messagePart), "Unexpected rejection: " + failure.getMessage());
            return;
        }
        catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            require(cause instanceof SecurityException, "Authentication failed for the wrong reason: " + cause);
            require(cause.getMessage().contains(messagePart), "Unexpected rejection: " + cause.getMessage());
            return;
        }
        throw new AssertionError("Untrusted process attachment was accepted");
    }

    private static void remoteDenied(String packageName, String permission) throws Exception {
        int uid = android.os.Process.myUid();
        require(uid != android.os.Process.SYSTEM_UID && uid != android.os.Process.SHELL_UID,
                "Remote rejection test requires an unprivileged process UID");
        IActivityManager am = IActivityManager.Stub.asInterface(ServiceManager.getService("activity"));
        android.permission.IPermissionManager permissions = android.permission.IPermissionManager
                .Stub.asInterface(ServiceManager.getService("permissionmgr"));
        require(am != null && permissions != null, "The test device services are unavailable");
        denied(() -> am.attachApplication(new ThreadStub(), Long.MAX_VALUE), "start sequence");
        // Granting another package's permission is a privileged mutation. It must
        // reject this unknown PID before looking up any claimed package identity.
        denied(() -> permissions.grantRuntimePermission(packageName, permission, "default:0", 0), "");
        System.out.println("IdentitySecurityTest: real Binder rejects forged attachment and unattached permission mutation");
    }

    private static int uid(String name) throws Exception {
        return (Integer) Class.forName("com.gesher.server.PackageUids")
                .getMethod("uidFor", String.class).invoke(null, name);
    }

    private static Properties read(File file) throws Exception {
        Properties values = new Properties();
        try (FileInputStream input = new FileInputStream(file)) { values.load(input); }
        return values;
    }

    private static void write(File file, Properties values) throws Exception {
        try (FileOutputStream output = new FileOutputStream(file)) { values.store(output, "test fixture"); }
    }

    private static void persistentUids(File root, boolean existingOnly) throws Exception {
        android.system.Os.setenv("ANDROID_DATA", root.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        File registry = new File(root, "system/package-uids");
        Properties before = existingOnly ? read(registry) : null;
        int first = uid("test.identity.alpha");
        int second = uid("test.identity.beta");
        require(first != second && first >= 10000 && second <= 19999,
                "Two installed package names share an app UID or exceed the app range");
        require(first == uid("test.identity.alpha") && second == uid("test.identity.beta"),
                "UID allocation was not stable");
        Properties persisted = read(registry);
        require(Integer.toString(first).equals(persisted.getProperty("test.identity.alpha"))
                        && Integer.toString(second).equals(persisted.getProperty("test.identity.beta")),
                "Allocated identities were not persisted");
        if (existingOnly) {
            require(before.equals(persisted), "Reopening the UID registry reassigned existing identities");
            System.out.println("IdentitySecurityTest: existing UID registry reopened without reassignment");
        } else {
            Properties corrupt = new Properties(); corrupt.putAll(persisted);
            corrupt.setProperty("test.identity.duplicate", Integer.toString(first));
            write(registry, corrupt);
            try {
                try { uid("test.identity.attacker"); throw new AssertionError("Duplicate UID registry accepted"); }
                catch (InvocationTargetException rejected) {
                    require(rejected.getCause() instanceof IllegalStateException,
                            "Corrupt registry did not fail closed");
                }
                require(read(registry).equals(corrupt), "Failed allocation rewrote a corrupt UID registry");
            } finally { write(registry, persisted); }
            require(uid("test.identity.gamma") != first && uid("test.identity.gamma") != second,
                    "A later package reused an existing package UID");
            System.out.println("IdentitySecurityTest: unique durable UIDs and corrupt-registry rejection passed");
        }
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        require(uid("test.identity.legacy.one") == 10000 && uid("test.identity.legacy.two") == 10000,
                "Legacy opt-out compatibility changed");
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
    }

    private static final class ThreadStub extends IApplicationThread.Default {
        private final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }

    @SuppressWarnings("unchecked")
    private static void processAuthentication(File fixtures) throws Exception {
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkClass.getMethod("parse", String.class)
                .invoke(null, new File(fixtures, "v2only.apk").getPath());
        int assignedUid = (Integer) apkClass.getMethod("uid").invoke(apk);
        require(assignedUid != Binder.getCallingUid(), "UID mismatch fixture accidentally uses the caller UID");
        String pkg = (String) apkClass.getField("packageName").get(apk);
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object pm = construct(pmClass, new Class<?>[] {java.util.Collection.class}, Collections.singletonList(apk));
        Object atm = construct(atmClass, new Class<?>[] {pmClass}, pm);
        Class<?> pendingClass = Class.forName(atmClass.getName() + "$PendingProcess");
        Map<Long, Object> pending = (Map<Long, Object>) field(atmClass, "mStarting").get(atm);
        List<Object> processes = (List<Object>) field(atmClass, "mProcesses").get(atm);
        Method attach = method(atmClass, "attachApplication", IApplicationThread.class, int.class, long.class);
        ThreadStub thread = new ThreadStub();

        denied(() -> attach.invoke(atm, thread, 888881, 913L), "start sequence");
        require(processes.isEmpty(), "Rejected unknown start registered a process");
        Object expected = construct(pendingClass, new Class<?>[] {String.class, String.class}, pkg, "identity test");
        field(pendingClass, "pid").setInt(expected, 888882);
        pending.put(914L, expected);
        denied(() -> attach.invoke(atm, thread, 888883, 914L), "start sequence");
        require(processes.isEmpty() && pending.get(914L) == expected,
                "A PID mismatch consumed the legitimate pending start");

        field(pendingClass, "pid").setInt(expected, Binder.getCallingPid());
        denied(() -> attach.invoke(atm, thread, Binder.getCallingPid(), 914L), "UID");
        require(processes.isEmpty() && pending.get(914L) == expected,
                "A UID mismatch registered a process or consumed a valid pending start");

        Class<?> processClass = Class.forName(atmClass.getName() + "$ProcessRecord");
        Object record = construct(processClass, new Class<?>[] {atmClass, int.class, IApplicationThread.class},
                atm, 888884, thread);
        field(processClass, "packageName").set(record, pkg);
        processes.add(record);
        Class<?> identityClass = Class.forName("com.gesher.server.CallerIdentity");
        Object identity = construct(identityClass, new Class<?>[] {pmClass, atmClass}, pm, atm);
        Method forPid = identityClass.getMethod("packageForPid", int.class);
        require(pkg.equals(forPid.invoke(identity, 888884)), "Attached PID did not map to its own package");
        require(forPid.invoke(identity, 888885) == null, "Unknown PID received an app identity");
        field(processClass, "exiting").setBoolean(record, true);
        require(forPid.invoke(identity, 888884) == null, "Exiting process retained its app identity");
        System.out.println("IdentitySecurityTest: guessed start, PID/UID mismatch and stale-PID mapping denied");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        if (args.length == 3 && args[0].equals("remote-denied")) {
            remoteDenied(args[1], args[2]);
        } else if (args.length == 2 && (args[0].equals("uid-write") || args[0].equals("uid-read"))) {
            persistentUids(new File(args[1]), args[0].equals("uid-read"));
        } else {
            require(args.length == 1, "Pass the signing fixtures directory");
            AssetManager.createSystemAssetsInZygoteLocked(false,
                    System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
            File data = Files.createTempDirectory("gesher-identity-security-").toFile();
            persistentUids(data, false);
            processAuthentication(new File(args[0]));
        }
        System.out.println("IdentitySecurityTest all passed");
    }
}
