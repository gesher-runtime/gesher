/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.IIntentSender;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageInstallerSession;
import android.content.pm.IPackageInstallerCallback;
import android.content.pm.PackageInstaller;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.IBinder;
import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

/** Offline server-local consent/owner injection; no app code, Google traffic, or real consent UI. */
public final class PackageInstallerSessionTest {
    private static int checks;
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason); checks++;
    }
    private static Object construct(Class<?> type, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(types); c.setAccessible(true); return c.newInstance(args);
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args); m.setAccessible(true); return m;
    }
    private interface Action { void run() throws Exception; }
    private static void rejected(Action action, Class<? extends Throwable> expected) throws Exception {
        try { action.run(); }
        catch (Throwable e) {
            if (e instanceof InvocationTargetException) e = ((InvocationTargetException)e).getCause();
            require(expected.isInstance(e), "Wrong failure class: " + e.getClass().getName()); return;
        }
        throw new AssertionError("Operation should fail: " + expected.getSimpleName());
    }
    private static IntentSender receiver(AtomicReference<Intent> result) {
        return new IntentSender((IIntentSender)new IIntentSender.Stub() {
            @Override public void send(int code, Intent intent, String type, IBinder token,
                    IIntentReceiver finished, String permission, Bundle options) { result.set(intent); }
        });
    }
    private static int create(IPackageInstaller installer, String owner, String pkg) throws Exception {
        PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        p.appPackageName = pkg;
        // A store can ask for silent grants; this implementation must ignore that request.
        p.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        p.installFlags = 0x00000100; // AOSP INSTALL_GRANT_RUNTIME_PERMISSIONS, hidden from this framework export.
        return installer.createSession(p, owner, null, 0);
    }
    private static void stage(IPackageInstaller installer, int id, File apk, String name) throws Exception {
        PackageInstaller.Session session = new PackageInstaller.Session(installer.openSession(id));
        try (OutputStream out = session.openWrite(name, 0, apk.length()); FileInputStream in = new FileInputStream(apk)) {
            byte[] buffer = new byte[4096];
            for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
            session.fsync(out);
        }
    }
    private static void boundedBridge(File root) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.InstallFileBridge");
        Class<?> reserve = Class.forName(type.getName() + "$Reserve");
        Class<?> transport = Class.forName(type.getName() + "$Transport");
        Object growth = Proxy.newProxyInstance(reserve.getClassLoader(), new Class<?>[]{reserve},
                (proxy, called, values) -> null);
        Object sockets = Proxy.newProxyInstance(transport.getClassLoader(), new Class<?>[]{transport},
                (proxy, called, values) -> {
                    if (called.getName().equals("create")) return android.os.ParcelFileDescriptor.createSocketPair();
                    ((android.os.ParcelFileDescriptor)values[0]).close(); return null;
                });
        AtomicReference<Boolean> completion = new AtomicReference<>();
        File target = new File(root, "bounded-synthetic.bin");
        Thread bridge = (Thread) construct(type,
                new Class<?>[]{File.class, long.class, long.class, reserve, java.util.function.Consumer.class, transport},
                target, 0L, 8L, growth, (java.util.function.Consumer<Boolean>)completion::set, sockets);
        android.os.ParcelFileDescriptor client = (android.os.ParcelFileDescriptor)
                method(type, "client").invoke(bridge);
        bridge.start();
        rejected(() -> {
            try (OutputStream output = new android.os.FileBridge.FileBridgeOutputStream(client)) {
                output.write(new byte[9]);
            }
        }, java.io.IOException.class);
        bridge.join(3000);
        require(!bridge.isAlive() && Boolean.FALSE.equals(completion.get()) && target.length() <= 8,
                "Bounded bridge accepted bytes past its limit or leaked its worker");
    }
    private static final class DeathBinder extends android.os.Binder {
        boolean alive = true;
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
        @Override public boolean isBinderAlive() { return alive; }
        @Override public void linkToDeath(IBinder.DeathRecipient recipient, int flags) { recipients.add(recipient); }
        @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) { return recipients.remove(recipient); }
        void die() {
            alive = false;
            for (IBinder.DeathRecipient recipient : new ArrayList<>(recipients)) recipient.binderDied();
        }
    }
    private static void callbacks(IPackageInstaller service, Class<?> serviceClass) throws Exception {
        DeathBinder binder = new DeathBinder();
        IPackageInstallerCallback a = IPackageInstallerCallback.Stub.asInterface(binder);
        IPackageInstallerCallback b = IPackageInstallerCallback.Stub.asInterface(binder);
        require(a != b && a.asBinder() == b.asBinder(), "Fixture did not produce distinct AIDL Proxy objects");
        Field field = serviceClass.getDeclaredField("mCallbacks"); field.setAccessible(true);
        for (int n = 0; n < 70; n++) {
            service.registerCallback(a, 0); service.unregisterCallback(b);
        }
        require(((Map<?,?>)field.get(service)).isEmpty() && binder.recipients.isEmpty(),
                "Cross-Proxy unregister leaked callbacks or death recipients");
        service.registerCallback(a, 0); service.registerCallback(b, 0);
        require(((Map<?,?>)field.get(service)).size() == 1 && binder.recipients.size() == 1,
                "Duplicate Binder registration leaked callbacks");
        binder.die();
        require(((Map<?,?>)field.get(service)).isEmpty() && binder.recipients.isEmpty(),
                "Callback Binder death did not release its registration");
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-package-sessions-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        boundedBridge(data);
        File apk = new File(args[0], "v2only.apk");
        android.content.pm.parsing.result.ParseResult<android.content.pm.parsing.ApkLite> lite =
                android.content.pm.parsing.ApkLiteParseUtils.parseApkLite(
                    android.content.pm.parsing.result.ParseTypeImpl.forParsingWithoutPlatformCompat(), apk, 0);
        require(lite.isSuccess(), "Synthetic APK-lite parser failed: " + lite.getErrorMessage());
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> installerClass = Class.forName("com.gesher.server.Installer");
        Class<?> serviceClass = Class.forName("com.gesher.server.PackageInstallerService");
        Class<?> consentClass = Class.forName(serviceClass.getName() + "$Consent");
        Class<?> transportClass = Class.forName("com.gesher.server.InstallFileBridge$Transport");
        Object parsed = apkClass.getMethod("parse", String.class).invoke(null, apk.getPath());
        String pkg = (String) apkClass.getField("packageName").get(parsed);
        Object pm = pmClass.getConstructor(Collection.class).newInstance(Collections.singletonList(parsed));
        Object atm = construct(atmClass, new Class<?>[]{pmClass}, pm);
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Class<?> amClass = Class.forName("com.gesher.server.ActivityManagerService");
        construct(amClass, new Class<?>[]{atmClass, pmClass, notifications}, atm, pm,
                construct(notifications, new Class<?>[0]));
        Object backend = construct(installerClass, new Class<?>[]{pmClass, atmClass, Map.class},
                pm, atm, new LinkedHashMap<>());
        AtomicReference<String> caller = new AtomicReference<>(pkg);
        AtomicReference<Boolean> allow = new AtomicReference<>(false);
        AtomicInteger prompts = new AtomicInteger();
        AtomicReference<Runnable> duringConsent = new AtomicReference<>();
        List<Runnable> jobs = new ArrayList<>();
        Object consent = Proxy.newProxyInstance(consentClass.getClassLoader(), new Class<?>[]{consentClass},
                (proxy, called, values) -> {
                    require(pkg.equals(values[1]), "Consent did not describe verified package");
                    prompts.incrementAndGet();
                    Runnable change = duringConsent.getAndSet(null);
                    if (change != null) change.run();
                    return allow.get();
                });
        Object transport = Proxy.newProxyInstance(transportClass.getClassLoader(), new Class<?>[]{transportClass},
                (proxy, called, values) -> {
                    if (called.getName().equals("create")) return android.os.ParcelFileDescriptor.createSocketPair();
                    ((android.os.ParcelFileDescriptor)values[0]).close(); return null;
                });
        IPackageInstaller service = (IPackageInstaller) construct(serviceClass,
                new Class<?>[]{pmClass, Executor.class, consentClass, Supplier.class, transportClass}, pm,
                (Executor)jobs::add, consent, (Supplier<String>)caller::get, transport);
        method(serviceClass, "attach", atmClass, installerClass).invoke(service, atm, backend);
        callbacks(service, serviceClass);
        rejected(() -> create(service, "not.the.caller", pkg), SecurityException.class);
        PackageInstaller.SessionParams advanced = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
        rejected(() -> service.createSession(advanced, pkg, null, 0), UnsupportedOperationException.class);
        PackageInstaller.SessionParams apex = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        apex.setInstallAsApex();
        rejected(() -> service.createSession(apex, pkg, null, 0), UnsupportedOperationException.class);
        int first = create(service, pkg, pkg);
        IPackageInstallerSession held = service.openSession(first);
        require(service.getSessionInfo(first).getRequireUserAction()
                == PackageInstaller.SessionParams.USER_ACTION_REQUIRED, "Caller bypassed consent requirement");
        require(held.getInstallFlags() == 0, "Caller selected privileged installation flags");
        caller.set("foreign.application");
        rejected(() -> service.openSession(first), SecurityException.class);
        rejected(held::getNames, SecurityException.class);
        require(service.getAllSessions(0).getList().isEmpty(), "Foreign session metadata leaked");
        rejected(() -> service.setPermissionsResult(first, true), SecurityException.class);
        caller.set(pkg);
        rejected(() -> held.openWrite("../outside.apk", 0, 12), IllegalArgumentException.class);
        rejected(() -> held.openWrite("C:\\outside.apk", 0, 12), IllegalArgumentException.class);
        rejected(() -> held.openWrite("base.apk", -1, 12), IllegalArgumentException.class);
        rejected(() -> held.openWrite("base.apk", 0, 1024L*1024*1024+1), IllegalArgumentException.class);
        rejected(() -> held.stageViaHardLink(apk.getPath()), SecurityException.class);
        stage(service, first, apk, "download-3492.apk");
        try (android.os.ParcelFileDescriptor read = held.openRead("download-3492.apk")) {
            byte[] actual = new android.os.ParcelFileDescriptor.AutoCloseInputStream(read).readAllBytes();
            require(Arrays.equals(actual, Files.readAllBytes(apk.toPath())), "FileBridge corrupted staged APK bytes");
        }
        AtomicReference<Intent> result = new AtomicReference<>();
        held.commit(receiver(result), false);
        require(result.get() == null && jobs.size() == 1, "Commit did not run asynchronously");
        rejected(() -> held.openWrite("new.apk", 0, 2), IllegalStateException.class);
        jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99)
                == PackageInstaller.STATUS_FAILURE_ABORTED && prompts.get() == 1,
                "Host denial did not produce honest aborted status: "
                        + result.get().getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        require(pmClass.getMethod("get", String.class).invoke(pm, pkg) == parsed,
                "Denied install replaced existing code");
        require(service.getSessionInfo(first) == null, "Completed denied session survived");
        rejected(held::getNames, IllegalStateException.class);

        // Stale installer identity after asynchronous capture cannot inherit an approval.
        int stale = create(service, pkg, pkg); stage(service, stale, apk, "base.apk");
        service.openSession(stale).commit(receiver(result), false);
        Object replacement = apkClass.getMethod("parse", String.class).invoke(null, apk.getPath());
        method(pmClass, "add", apkClass).invoke(pm, replacement); allow.set(true);
        jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99)
                == PackageInstaller.STATUS_FAILURE_BLOCKED && prompts.get() == 1,
                "Stale installer reached consent or install");

        // Signed production Installer path, synthetic host approval, no application code executes.
        caller.set("android");
        int success = create(service, "android", pkg); stage(service, success, apk, "download.apk");
        service.openSession(success).commit(receiver(result), false); jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_SUCCESS,
                "Approved signed APK did not install through real backend");
        Object installed = pmClass.getMethod("get", String.class).invoke(pm, pkg);
        require(installed != replacement && (Boolean)apkClass.getMethod("hasVerifiedSigningIdentity").invoke(installed),
                "Success did not publish verified installed code");
        String installedPath = (String)apkClass.getField("apkPath").get(installed);
        require(Arrays.equals(Files.readAllBytes(new File(installedPath).toPath()), Files.readAllBytes(apk.toPath())),
                "Published bytes differ from approved APK");

        int raced = create(service, "android", pkg); stage(service, raced, apk, "base.apk");
        Object changed = apkClass.getMethod("parse", String.class).invoke(null, apk.getPath());
        duringConsent.set(() -> {
            try { method(pmClass, "add", apkClass).invoke(pm, changed); }
            catch (Exception e) { throw new AssertionError(e); }
        });
        service.openSession(raced).commit(receiver(result), false); jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_FAILURE_BLOCKED,
                "Approval survived a target-code replacement inside the consent dialog");
        require(pmClass.getMethod("get", String.class).invoke(pm, pkg) == changed,
                "Stale approval overwrote the changed target package");

        int unsigned = create(service, "android", pkg);
        stage(service, unsigned, new File(args[0], "unsigned.apk"), "base.apk");
        service.openSession(unsigned).commit(receiver(result), false); jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_FAILURE_BLOCKED,
                "Unsigned store package was accepted");
        require(pmClass.getMethod("get", String.class).invoke(pm, pkg) == changed,
                "Rejected unsigned install changed package");
        int abandoned = create(service, "android", pkg);
        IPackageInstallerSession oldHandle = service.openSession(abandoned); oldHandle.abandon();
        rejected(oldHandle::getNames, IllegalStateException.class);
        int[] limited = new int[4];
        for (int n = 0; n < limited.length; n++) limited[n] = create(service, "android", pkg);
        rejected(() -> create(service, "android", pkg), IllegalStateException.class);
        for (int id : limited) service.abandonSession(id);
        caller.set(pkg);
        IPackageInstallerSession[] staleHandles = new IPackageInstallerSession[4];
        for (int n = 0; n < staleHandles.length; n++) {
            staleHandles[n] = service.openSession(create(service, pkg, pkg));
        }
        Object updatedOwner = apkClass.getMethod("parse", String.class).invoke(null, apk.getPath());
        method(pmClass, "add", apkClass).invoke(pm, updatedOwner);
        int recovered = create(service, pkg, pkg);
        require(service.getMySessions(pkg, 0).getList().size() == 1,
                "Installer update did not release stale sessions and their quota");
        for (IPackageInstallerSession staleHandle : staleHandles) rejected(staleHandle::getNames, SecurityException.class);
        service.abandonSession(recovered);
        System.out.println("PackageInstallerSessionTest all passed: " + checks + " checks");
    }
}
