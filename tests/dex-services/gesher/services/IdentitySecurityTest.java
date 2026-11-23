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
 * "revoke-post-without-kill PACKAGE" runs as the shell UID against an existing test
 * device and uses Android's test-only POST_NOTIFICATIONS revoke that skips killUid.
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
        android.app.usage.IUsageStatsManager usage = android.app.usage.IUsageStatsManager.Stub
                .asInterface(ServiceManager.getService("usagestats"));
        require(am != null && permissions != null && usage != null,
                "The test device services are unavailable");
        denied(() -> am.attachApplication(new ThreadStub(), Long.MAX_VALUE), "start sequence");
        // Granting another package's permission is a privileged mutation. It must
        // reject this unknown PID before looking up any claimed package identity.
        denied(() -> permissions.grantRuntimePermission(packageName, permission, "default:0", 0), "");
        denied(() -> usage.queryEventsForPackage(0, Long.MAX_VALUE, packageName), "");
        denied(() -> usage.queryEvents(0, Long.MAX_VALUE, packageName), "");
        System.out.println("IdentitySecurityTest: real Binder rejects forged attachment and unattached permission mutation");
    }

    private static void revokePostWithoutKill(String packageName) throws Exception {
        require(android.os.Process.myUid() == android.os.Process.SHELL_UID,
                "The no-kill revoke fixture must run with the shell UID");
        android.permission.IPermissionManager permissions = android.permission.IPermissionManager
                .Stub.asInterface(ServiceManager.getService("permissionmgr"));
        require(permissions != null, "The test device permission service is unavailable");
        permissions.revokePostNotificationPermissionWithoutKillForTest(packageName, 0);
        require(permissions.checkPermission(packageName, android.Manifest.permission.POST_NOTIFICATIONS,
                "default:0", 0) == android.content.pm.PackageManager.PERMISSION_DENIED,
                "POST_NOTIFICATIONS remained granted after the no-kill revoke");
        System.out.println("IdentitySecurityTest: POST_NOTIFICATIONS revoked without killing " + packageName);
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
        int leg1 = uid("test.identity.legacy.one");
        int leg2 = uid("test.identity.legacy.two");
        require(leg1 != leg2 && leg1 >= 10000 && leg2 <= 19999,
                "Per-package UIDs must be distinct even with spoofing disabled");
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
    }

    private static final class ThreadStub extends IApplicationThread.Default {
        private final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }

    /** Use real accepted attachment so PID ownership and death cleanup are exercised. */
    private static Object attachProcess(Object atm, Method attach, List<Object> processes,
            int pid, String packageName) throws Exception {
        int before = processes.size();
        attach.invoke(atm, new ThreadStub(), pid, 0L);
        require(processes.size() == before + 1, "Attachment discarded a record before death cleanup");
        Object process = processes.get(before);
        synchronized (atm) {
            field(process.getClass(), "packageName").set(process, packageName);
            field(process.getClass(), "processName").set(process, packageName);
        }
        return process;
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
        Object expected = construct(pendingClass, new Class<?>[] {String.class, String.class, String.class}, pkg, pkg, "identity test");
        field(pendingClass, "owner").set(expected, apk);
        field(pendingClass, "enabledLifetime").set(expected, method(apkClass, "enabledProcessLifetime").invoke(apk));
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
        Class<?> amClass = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        construct(amClass, new Class<?>[] {atmClass, pmClass, notifications},
                atm, pm, construct(notifications, new Class<?>[0]));
        Method caller = method(atmClass, "callerProcess", int.class);
        Method unassigned = method(atmClass, "unassignedProcess", int.class);
        Method died = method(atmClass, "processDied", processClass);
        Field exiting = field(processClass, "exiting");
        Field dead = field(processClass, "dead");
        Field superseded = field(processClass, "pidSuperseded");
        Field recover = field(processClass, "recoverAfterPidSupersession");
        int pid = 888884;
        Object record = attachProcess(atm, attach, processes, pid, pkg);
        Class<?> identityClass = Class.forName("com.gesher.server.CallerIdentity");
        Object identity = construct(identityClass, new Class<?>[] {pmClass, atmClass}, pm, atm);
        Method forPid = identityClass.getMethod("packageForPid", int.class);
        require(pkg.equals(forPid.invoke(identity, pid)), "Attached PID did not map to its own package");
        require(caller.invoke(atm, pid) == record, "Attached process lost lifecycle admission");
        require(forPid.invoke(identity, 888885) == null, "Unknown PID received an app identity");
        // Rejected attachments must not claim an already-owned PID, including a
        // valid pending sequence presented with the wrong PID or Binder UID.
        denied(() -> attach.invoke(atm, thread, pid, 913L), "start sequence");
        field(pendingClass, "pid").setInt(expected, pid + 1);
        denied(() -> attach.invoke(atm, thread, pid, 914L), "start sequence");
        field(pendingClass, "pid").setInt(expected, pid);
        denied(() -> attach.invoke(atm, thread, pid, 914L), "UID");
        require(processes.size() == 1 && processes.get(0) == record
                        && pending.get(914L) == expected && !superseded.getBoolean(record)
                        && !exiting.getBoolean(record) && caller.invoke(atm, pid) == record
                        && pkg.equals(forPid.invoke(identity, pid)),
                "Rejected attachment changed the existing PID owner or consumed a pending start");
        // Told to exit but still running (onTerminate, queued main-looper work): Android
        // keeps its Binder identity until the process is gone, so services must not turn
        // its last calls into SecurityExceptions.
        exiting.setBoolean(record, true);
        require(pkg.equals(forPid.invoke(identity, pid)), "Exiting live process lost its app identity");
        require(caller.invoke(atm, pid) == null, "Exiting process received new lifecycle work");
        // Supersession must also retire an older record that has not yet been told to exit.
        exiting.setBoolean(record, false);
        Object reused = attachProcess(atm, attach, processes, pid, "test.identity.reused");
        require(superseded.getBoolean(record) && exiting.getBoolean(record)
                        && recover.getBoolean(record) && !dead.getBoolean(record) && processes.contains(record),
                "PID replacement did not preserve the old record solely for death cleanup");
        require("test.identity.reused".equals(forPid.invoke(identity, pid))
                        && caller.invoke(atm, pid) == reused,
                "An older record shadowed the latest attached PID owner");
        exiting.setBoolean(reused, true);
        require("test.identity.reused".equals(forPid.invoke(identity, pid)),
                "Exiting replacement revived the previous PID owner's identity");
        require(caller.invoke(atm, pid) == null, "Exiting replacement restored older lifecycle admission");
        dead.setBoolean(reused, true);
        require(forPid.invoke(identity, pid) == null && caller.invoke(atm, pid) == null,
                "Dead replacement revived a previous PID owner");
        died.invoke(atm, reused);
        require(processes.contains(record) && !processes.contains(reused)
                        && forPid.invoke(identity, pid) == null && caller.invoke(atm, pid) == null,
                "Removing the latest owner revived a superseded process");

        Object oldUnbound = attachProcess(atm, attach, processes, pid, null);
        Object unbound = attachProcess(atm, attach, processes, pid, null);
        require(recover.getBoolean(record), "Later PID claims erased the old process's recovery intent");
        require(forPid.invoke(identity, pid) == null && caller.invoke(atm, pid) == unbound,
                "An unbound attachment inherited an older record instead of its own bootstrap state");
        synchronized (atm) {
            require(unassigned.invoke(atm, pid) == unbound && unassigned.invoke(atm, 0) == unbound,
                    "Unassigned-process selection returned a superseded attachment");
            dead.setBoolean(unbound, true);
            require(unassigned.invoke(atm, pid) == null && unassigned.invoke(atm, 0) == null,
                    "Dead unbound owner or its predecessor received a package assignment");
            dead.setBoolean(unbound, false);
        }
        exiting.setBoolean(unbound, true);
        require(forPid.invoke(identity, pid) == null && caller.invoke(atm, pid) == null,
                "Exiting unbound owner revived an older process");
        synchronized (atm) {
            require(unassigned.invoke(atm, pid) == null && unassigned.invoke(atm, 0) == null,
                    "Exiting unbound owner or its predecessor received a package assignment");
        }
        died.invoke(atm, unbound);
        require(forPid.invoke(identity, pid) == null && caller.invoke(atm, pid) == null,
                "Removing an unbound owner revived an older process");

        Object latest = attachProcess(atm, attach, processes, pid, pkg);
        synchronized (atm) {
            require(unassigned.invoke(atm, pid) == null && unassigned.invoke(atm, 0) == null,
                    "A bound replacement left its superseded unbound predecessor assignable");
        }
        died.invoke(atm, oldUnbound);
        died.invoke(atm, record);
        require(dead.getBoolean(record) && !processes.contains(record)
                        && !dead.getBoolean(latest) && !superseded.getBoolean(latest)
                        && pkg.equals(forPid.invoke(identity, pid)) && caller.invoke(atm, pid) == latest,
                "Delayed cleanup of the old process revoked its replacement");
        died.invoke(atm, latest);
        require(processes.isEmpty() && forPid.invoke(identity, pid) == null
                        && caller.invoke(atm, pid) == null,
                "Removed process retained identity or lifecycle admission");
        System.out.println("IdentitySecurityTest: guessed start, PID/UID mismatch and stale-PID mapping denied;"
                + " exiting owner keeps identity and superseded owners never revive");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        if (args.length == 3 && args[0].equals("remote-denied")) {
            remoteDenied(args[1], args[2]);
        } else if (args.length == 2 && args[0].equals("revoke-post-without-kill")) {
            revokePostWithoutKill(args[1]);
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
