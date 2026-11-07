/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageInstallerSession;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInstaller;
import android.os.Binder;
import android.os.ParcelFileDescriptor;
import android.os.ServiceManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

/** Actual separate-process stream/cancellation/role checks. Never commits or requests consent. */
public final class PackageInstallerIpcTest {
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
    }
    private static void nativeRoles() throws Exception {
        Class<?> transport = Class.forName("com.gesher.server.InstallPipeTransport");
        Method create = transport.getDeclaredMethod("create"); create.setAccessible(true);
        Method cancel = transport.getDeclaredMethod("nativeCancelIo", int.class); cancel.setAccessible(true);
        for (Method method : new Method[]{create, cancel}) {
            try {
                if (method == create) method.invoke(null); else method.invoke(null, -1);
                throw new AssertionError("Non-server native installation operation accepted");
            } catch (InvocationTargetException expected) {
                require(expected.getCause() instanceof SecurityException, "Wrong native role failure");
            }
        }
    }
    private static IPackageInstaller remote() throws Exception {
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        require(pm != null && !(pm.asBinder() instanceof Binder), "Package manager is not remote");
        IPackageInstaller installer = pm.getPackageInstaller();
        require(installer != null && !(installer.asBinder() instanceof Binder), "Installer is not remote");
        int serverPid = Integer.parseInt(Files.readString(new File(System.getenv("ANDROID_DATA"),
                "system/gesher-device.ready").toPath()).trim());
        require(serverPid != android.os.Process.myPid(), "Fixture client and server share a PID");
        return installer;
    }
    private static PackageInstaller.SessionParams params(long size) {
        PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        p.sizeBytes = size;
        p.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        return p;
    }
    private static void stream(IPackageInstaller installer, File apk) throws Exception {
        int id = installer.createSession(params(apk.length()), "android", null, 0);
        try {
            IPackageInstallerSession remote = installer.openSession(id);
            require(!(remote.asBinder() instanceof Binder), "Session is not remote");
            PackageInstaller.Session session = new PackageInstaller.Session(remote);
            try (OutputStream output = session.openWrite("0", 0, apk.length());
                    FileInputStream input = new FileInputStream(apk)) {
                byte[] buffer = new byte[4096];
                for (int n; (n = input.read(buffer)) != -1;) output.write(buffer, 0, n);
                session.fsync(output);
            }
            try (ParcelFileDescriptor input = remote.openRead("0")) {
                byte[] actual = new ParcelFileDescriptor.AutoCloseInputStream(input).readAllBytes();
                require(Arrays.equals(actual, Files.readAllBytes(apk.toPath())), "Streamed APK bytes differ");
            }
            byte[] dm = {0x50, 0x4b, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            try (OutputStream output = session.openWrite("0.dm", 0, dm.length)) { output.write(dm); }
            try (ParcelFileDescriptor input = remote.openRead("0.dm")) {
                require(Arrays.equals(dm, new ParcelFileDescriptor.AutoCloseInputStream(input).readAllBytes()),
                        "Remote DM sidecar stream differs");
            }
            android.os.PersistableBundle metadata = new android.os.PersistableBundle();
            metadata.putString("fixture", "synthetic remote metadata");
            session.setAppMetadata(metadata);
            require("synthetic remote metadata".equals(session.getAppMetadata().getString("fixture")),
                    "Remote metadata FileBridge/descriptor readback failed");
            session.setChecksums("0", java.util.Collections.singletonList(new android.content.pm.Checksum(8,
                    java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(apk.toPath())))), null);
            session.setAppMetadata(null);
            require(session.getAppMetadata().isEmpty() && remote.getAppMetadataFd() == null,
                    "Remote metadata removal failed");
            require(Arrays.asList(session.getNames()).containsAll(Arrays.asList("0", "0.dm")),
                    "Remote arbitrary-name APK/DM enumeration failed");
        } finally { installer.abandonSession(id); }
        System.out.println("PackageInstallerIpcTest: remote stream fsync close readback passed");
        System.out.println("PackageInstallerIpcTest: remote arbitrary names, DM and metadata APIs passed");
    }
    private static void cancellation(IPackageInstaller installer) throws Exception {
        long maximum = 0;
        for (int round = 0; round < 20; round++) {
            int id = installer.createSession(params(100), "android", null, 0);
            ParcelFileDescriptor retained = installer.openSession(id).openWrite("pending.apk", 0, 100);
            AtomicBoolean released = new AtomicBoolean(false);
            Thread watchdog = new Thread(() -> {
                try {
                    Thread.sleep(5000);
                    if (released.compareAndSet(false, true)) retained.close();
                } catch (Exception ignored) { }
            }, "installer-retained-fd-watchdog");
            watchdog.setDaemon(true); watchdog.start();
            try {
                // Exercise both an already blocked read and the IO-start race.
                if (round % 2 == 0) Thread.sleep(100);
                long began = System.nanoTime();
                installer.abandonSession(id);
                long elapsed = (System.nanoTime() - began) / 1000000;
                maximum = Math.max(maximum, elapsed);
                require(elapsed < 2500 && !released.get() && retained.getFd() >= 0,
                        "Abandon waited for client descriptor closure");
                File staging = new File(System.getenv("ANDROID_DATA"), "system/install-sessions");
                long deadline = System.currentTimeMillis() + 2500;
                String[] entries;
                while ((entries = staging.list()) != null && entries.length != 0
                        && System.currentTimeMillis() < deadline) Thread.sleep(10);
                require(entries != null && entries.length == 0 && !released.get(),
                        "Abandoned writer or staging files did not retire with client FD retained");
            } finally {
                if (released.compareAndSet(false, true)) retained.close();
            }
        }
        System.out.println("PackageInstallerIpcTest: retained descriptor cancellation passed rounds=20 max_ms=" + maximum);
    }
    private static void untrusted(IPackageInstaller installer) throws Exception {
        int uid = android.os.Process.myUid();
        require(uid != 0 && uid != 1000 && uid != 2000, "Caller-denial fixture is privileged");
        for (String claimed : new String[]{"android", "com.android.vending", "com.gesher.fixture"}) {
            try {
                installer.createSession(params(100), claimed, null, 0);
                throw new AssertionError("Unattached caller created an installation session");
            } catch (SecurityException expected) { }
        }
        try {
            installer.getAllSessions(0);
            throw new AssertionError("Unattached caller enumerated sessions");
        } catch (SecurityException expected) { }
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        try {
            pm.getAppMetadataFd("android", 0);
            throw new AssertionError("Unattached caller read app metadata");
        } catch (SecurityException expected) { }
        try {
            pm.getAppMetadataSource("android", 0);
            throw new AssertionError("Unattached caller read app metadata source");
        } catch (SecurityException expected) { }
        com.gesher.server.GesherService.Client host = com.gesher.server.GesherService.Client.find();
        require(host != null, "Host control service missing");
        try {
            host.startActivityAndWait(new android.content.Intent().setPackage("android"));
            throw new AssertionError("Unattached caller acquired a host launch receipt");
        } catch (SecurityException expected) { }
        System.out.println("PackageInstallerIpcTest: unattached caller and native role denials passed");
    }
    public static void main(String[] args) throws Exception {
        require(args.length >= 1, "Pass stream, cancel, or untrusted");
        System.loadLibrary("android_runtime");
        nativeRoles();
        IPackageInstaller installer = remote();
        if (args[0].equals("untrusted")) untrusted(installer);
        else {
            require(android.os.Process.myUid() == 2000, "Positive fixture requires explicit host shell authority");
            if (args[0].equals("stream")) { require(args.length == 2, "Pass the signed APK fixture"); stream(installer, new File(args[1])); }
            else if (args[0].equals("cancel")) cancellation(installer);
            else throw new IllegalArgumentException("Unknown installer fixture mode");
        }
        System.out.println("PackageInstallerIpcTest all passed");
    }
}
