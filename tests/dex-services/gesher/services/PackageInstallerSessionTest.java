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
    private static void stageBytes(IPackageInstaller service, int id, String name, byte[] bytes) throws Exception {
        PackageInstaller.Session api = new PackageInstaller.Session(service.openSession(id));
        try (OutputStream output = api.openWrite(name, 0, bytes.length)) {
            output.write(bytes); api.fsync(output);
        }
    }
    private static byte[] dm(String pkg, long version) throws Exception {
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(bytes)) {
            zip.putNextEntry(new java.util.zip.ZipEntry("manifest.json"));
            zip.write(("{\"packageName\":\"" + pkg + "\",\"versionCode\":" + version + "}")
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8));
            zip.closeEntry();
        }
        return bytes.toByteArray();
    }
    private static void metadata(IPackageInstaller service, File fixtures, File apk, String pkg,
            long version, Object pm, Class<?> apkClass, Class<?> pmClass, AtomicReference<String> caller,
            AtomicReference<Boolean> allow, AtomicInteger prompts, List<Runnable> jobs,
            AtomicReference<Intent> result) throws Exception {
        caller.set("android"); allow.set(true);
        int id = create(service, "android", pkg);
        final IPackageInstallerSession naming = service.openSession(id);
        require(naming.fetchPackageNames().isEmpty(), "Session treated an unverified package hint as parsed APK identity");
        for (String bad : new String[]{"NUL", "con.apk", "aux.dm", "a.", "../file", "a/b", "C:file"}) {
            rejected(() -> naming.openWrite(bad, 0, 1), IllegalArgumentException.class);
        }
        rejected(() -> naming.openWrite("app.metadata", 0, 1), IllegalArgumentException.class);
        rejected(() -> naming.openWrite("base.digests", 0, 1), IllegalArgumentException.class);
        rejected(() -> naming.openWrite("base.apk.idsig", 0, 1), UnsupportedOperationException.class);
        rejected(() -> naming.openWrite("base.dm", 0, 64L * 1024 * 1024 + 1), IllegalArgumentException.class);
        stage(service, id, apk, "0"); // Public API permits arbitrary names with no .apk extension.
        require(naming.fetchPackageNames().equals(Collections.singletonList(pkg)), "Session did not parse the staged APK with an arbitrary filename");
        require(Arrays.asList(naming.getNames()).contains("0"), "Arbitrary APK filename disappeared");
        rejected(() -> naming.openWrite("0.digests", 0, 1), IllegalArgumentException.class);
        stageBytes(service, id, "Case", new byte[0]);
        rejected(() -> naming.openWrite("case", 0, 1), IllegalArgumentException.class);
        naming.abandon(); // The empty Case file must not become an ignored payload.

        id = create(service, "android", pkg);
        final IPackageInstallerSession session = service.openSession(id);
        byte[] hash = java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(apk.toPath()));
        byte[] signature = Files.readAllBytes(new File(fixtures, "checksum-signature.der").toPath());
        android.content.pm.Checksum[] digests = {new android.content.pm.Checksum(8, hash)};
        rejected(() -> session.setChecksums("0", new android.content.pm.Checksum[]{
                new android.content.pm.Checksum(8, new byte[1])}, null), IllegalArgumentException.class);
        rejected(() -> session.setChecksums("0", new android.content.pm.Checksum[]{digests[0], digests[0]}, null),
                IllegalArgumentException.class);
        rejected(() -> session.setChecksums("0", digests, new byte[]{1, 2, 3}), IllegalArgumentException.class);
        byte[] forged = signature.clone(); forged[forged.length - 1] ^= 1;
        rejected(() -> session.setChecksums("0", digests, forged), IllegalArgumentException.class);
        rejected(() -> session.setChecksums("0", new android.content.pm.Checksum[]{
                new android.content.pm.Checksum(8, new byte[32])}, signature), IllegalArgumentException.class);
        session.setChecksums("0", digests, signature);
        rejected(() -> session.setChecksums("0", digests, null), IllegalStateException.class);
        hash[0] ^= 1; signature[0] ^= 1; // The service must retain its own immutable copies.
        caller.set("foreign.application");
        rejected(session::openWriteAppMetadata, SecurityException.class);
        rejected(session::getAppMetadataFd, SecurityException.class);
        rejected(session::removeAppMetadata, SecurityException.class);
        rejected(() -> session.setChecksums("x", digests, null), SecurityException.class);
        caller.set("android");
        PackageInstaller.Session api = new PackageInstaller.Session(session);
        android.os.PersistableBundle metadata = new android.os.PersistableBundle();
        metadata.putString("fixture", "synthetic metadata"); metadata.putLong("long", Long.MAX_VALUE);
        api.setAppMetadata(metadata);
        require("synthetic metadata".equals(api.getAppMetadata().getString("fixture"))
                && api.getAppMetadata().getLong("long") == Long.MAX_VALUE, "SDK metadata round-trip failed");
        android.os.PersistableBundle shortMetadata = new android.os.PersistableBundle(); shortMetadata.putInt("short", 1);
        api.setAppMetadata(shortMetadata);
        require(api.getAppMetadata().size() == 1 && api.getAppMetadata().getInt("short") == 1,
                "Replacing metadata retained trailing old bytes");
        api.setAppMetadata(null);
        require(api.getAppMetadata().isEmpty() && session.getAppMetadataFd() == null,
                "Metadata removal did not remove stored bytes");
        try (OutputStream pending = new android.os.FileBridge.FileBridgeOutputStream(session.openWriteAppMetadata())) {
            rejected(session::seal, IllegalStateException.class);
            rejected(session::getAppMetadataFd, IllegalStateException.class);
            rejected(session::removeAppMetadata, IllegalStateException.class);
        }
        api.setAppMetadata(metadata);
        stage(service, id, apk, "0"); byte[] dexMetadata = dm(pkg, version); stageBytes(service, id, "0.dm", dexMetadata);
        Field sessions = service.getClass().getDeclaredField("mSessions"); sessions.setAccessible(true);
        Object serverSession = ((Map<?,?>)sessions.get(service)).get(id);
        Field staged = serverSession.getClass().getDeclaredField("stage"); staged.setAccessible(true);
        File directory = (File)staged.get(serverSession);
        method(Class.forName("com.gesher.server.InstallSessionMetadata"), "validateMetadata", File.class)
                .invoke(null, new File(directory, "app.metadata"));
        require(android.content.pm.parsing.ApkLiteParseUtils.parseApkLite(
                android.content.pm.parsing.result.ParseTypeImpl.forParsingWithoutPlatformCompat(),
                new File(directory, "0"), 0).isSuccess(), "Arbitrary-name APK failed preflight parsing");
        require(android.content.pm.dex.DexMetadataHelper.validateDexMetadataFile(
                android.content.pm.parsing.result.ParseTypeImpl.forParsingWithoutPlatformCompat(),
                new File(directory, "0.dm").getPath(), pkg, version).isSuccess(), "Valid DM failed preflight parsing");
        int before = prompts.get(); session.commit(receiver(result), false);
        rejected(() -> session.setChecksums("late", digests, null), IllegalStateException.class);
        rejected(session::openWriteAppMetadata, IllegalStateException.class);
        rejected(session::removeAppMetadata, IllegalStateException.class);
        jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_SUCCESS
                && prompts.get() == before + 1, "Metadata APK set did not require real consent/backend install: status="
                        + result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) + " message="
                        + result.get().getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                        + " prompts=" + prompts.get() + " before=" + before);
        Object installed = pmClass.getMethod("get", String.class).invoke(pm, pkg);
        File code = new File((String)apkClass.getField("apkPath").get(installed)).getParentFile();
        require(Arrays.equals(dexMetadata, Files.readAllBytes(new File(code, "base.dm").toPath())),
                "Associated DM was lost or not renamed with its APK");
        java.io.ByteArrayOutputStream expected = new java.io.ByteArrayOutputStream();
        java.io.DataOutputStream output = new java.io.DataOutputStream(expected); output.writeInt(8); output.writeInt(32);
        output.write(java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(apk.toPath())));
        require(Arrays.equals(expected.toByteArray(), Files.readAllBytes(new File(code, "base.digests").toPath()))
                && Arrays.equals(Files.readAllBytes(new File(fixtures, "checksum-signature.der").toPath()),
                        Files.readAllBytes(new File(code, "base.digests.signature").toPath())),
                "Installer advisory checksums/signature were forged, mutated or dropped");
        android.content.pm.IPackageManager manager = (android.content.pm.IPackageManager)pm;
        try (android.os.ParcelFileDescriptor fd = manager.getAppMetadataFd(pkg, 0)) {
            require(fd != null && "synthetic metadata".equals(android.os.PersistableBundle.readFromStream(
                    new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)).getString("fixture")),
                    "Published app metadata is not readable through the protected platform API");
        }
        require(manager.getAppMetadataSource(pkg, 0) == 2, "Installer metadata source was not reported truthfully");
        require(Arrays.equals((Object[])apkClass.getMethod("verifiedSignatures").invoke(installed),
                (Object[])apkClass.getMethod("verifySignatures", String.class).invoke(null, apk.getPath())),
                "Advisory checksum signer replaced the APK signing identity");

        for (int bad = 0; bad < 5; bad++) {
            id = create(service, "android", pkg); IPackageInstallerSession invalid = service.openSession(id);
            stage(service, id, bad == 3 ? new File(fixtures, "unsigned.apk") : apk, "0");
            if (bad == 0) stageBytes(service, id, "orphan.dm", dexMetadata);
            if (bad == 1) stageBytes(service, id, "0.dm", new byte[]{1, 2, 3});
            if (bad == 2) invalid.setChecksums("missing", digests, null);
            if (bad == 3) invalid.setChecksums("0", new android.content.pm.Checksum[]{
                    new android.content.pm.Checksum(8, java.security.MessageDigest.getInstance("SHA-256")
                            .digest(Files.readAllBytes(apk.toPath())))},
                    Files.readAllBytes(new File(fixtures, "checksum-signature.der").toPath()));
            if (bad == 4) try (OutputStream raw = new android.os.FileBridge.FileBridgeOutputStream(
                    invalid.openWriteAppMetadata())) { raw.write(new byte[]{0, 1, 2}); }
            before = prompts.get(); invalid.commit(receiver(result), false); jobs.remove(0).run();
            require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != PackageInstaller.STATUS_SUCCESS
                    && prompts.get() == before, "Invalid DM/checksum subject or signed-advisory unsigned APK reached consent: case="
                            + bad + " status=" + result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99)
                            + " prompts=" + prompts.get() + " before=" + before);
            require(pmClass.getMethod("get", String.class).invoke(pm, pkg) == installed,
                    "Invalid sidecar/checksum install changed published code");
        }
        id = create(service, "android", pkg);
        stage(service, id, new File(fixtures, "../identity-fixtures/rotated-no-data.apk"), "0");
        IPackageInstallerSession wrongSigner = service.openSession(id);
        wrongSigner.setChecksums("0", new android.content.pm.Checksum[]{new android.content.pm.Checksum(8,
                java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(apk.toPath())))},
                Files.readAllBytes(new File(fixtures, "checksum-signature.der").toPath()));
        wrongSigner.commit(receiver(result), false); jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_FAILURE_BLOCKED
                && pmClass.getMethod("get", String.class).invoke(pm, pkg) == installed,
                "A signed advisory checksum bypassed the real APK update signing/lineage checks");
        id = create(service, "android", pkg); IPackageInstallerSession oversized = service.openSession(id);
        rejected(() -> {
            try (OutputStream out = new android.os.FileBridge.FileBridgeOutputStream(oversized.openWriteAppMetadata())) {
                out.write(new byte[32_001]);
            }
        }, java.io.IOException.class);
        rejected(oversized::seal, IllegalStateException.class); oversized.abandon();

        id = create(service, "android", pkg); stage(service, id, apk, "0");
        service.openSession(id).commit(receiver(result), false); jobs.remove(0).run();
        require(result.get().getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == PackageInstaller.STATUS_SUCCESS,
                "Same APK without optional metadata failed");
        Object replacement = pmClass.getMethod("get", String.class).invoke(pm, pkg);
        File replacementCode = new File((String)apkClass.getField("apkPath").get(replacement)).getParentFile();
        require(!new File(replacementCode, "app.metadata").exists() && !new File(replacementCode, "base.digests").exists()
                && !new File(replacementCode, "base.dm").exists() && manager.getAppMetadataFd(pkg, 0) == null,
                "Same-content session incorrectly retained metadata from its previous install");
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
        metadata(service, new File(args[0]), apk, pkg, lite.getResult().getLongVersionCode(),
                pm, apkClass, pmClass, caller, allow, prompts, jobs, result);
        System.out.println("PackageInstallerSessionTest all passed: " + checks + " checks");
    }
}
