/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.IApplicationThread;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.os.Binder;
import android.os.UserHandle;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.io.File;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Actual PMS, APK verifier and registry; constructed manifests/catalog and host JNI only. */
public final class InstallSourceHost {
    private static Unsafe unsafe;
    private static int checks, nextPid = 850000;
    private static Path fixtures, data;
    private static final Map<ApkPackage, Integer> pids = new java.util.IdentityHashMap<>();
    private interface Call { void run() throws Exception; }
    private static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Call call, String why) throws Exception {
        try { call.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(why, failure);
            checks++; return;
        }
        throw new AssertionError(why + ": no exception");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static void as(ApkPackage pkg, Call call) throws Exception {
        as(pids.get(pkg), pkg.uid(), call);
    }
    private static void as(int pid, int uid, Call call) throws Exception {
        long saved = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL)); call.run(); }
        finally { Binder.restoreCallingIdentity(saved); }
    }
    private static ApkPackage pkg(String name, String fixture, String group, int sdk,
            String... queries) throws Exception {
        Path apk = fixtures.resolve(fixture);
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, apk.toString());
        parsed.setTargetSdkVersion(sdk); parsed.setEnabled(true);
        if (name.endsWith("bundled")) parsed.setForceQueryable(true);
        if (group != null) parsed.setSharedUserId(group);
        for (String query : queries) parsed.addQueriesPackage(query);
        if (name.endsWith("privileged")) parsed.addUsesPermission(
                new ParsedUsesPermissionImpl(Manifest.permission.INSTALL_PACKAGES, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed);
        set(result, "apkPath", apk.toFile().getCanonicalPath());
        set(result, "mDataDir", data.resolve("data").resolve(name).toString());
        set(result, "sharedLibraries", SharedLibraryGraph.EMPTY);
        set(result, "mUid", PackageUids.uidFor(name));
        return result;
    }
    @SuppressWarnings("unchecked")
    private static void attach(ActivityTaskManagerService atm, ApkPackage pkg) throws Exception {
        int pid = ++nextPid; pids.put(pkg, pid);
        as(pid, pkg.uid(), () -> {
            ActivityTaskManagerService.ProcessRecord process = atm.new ProcessRecord(pid,
                    new IApplicationThread.Default());
            process.packageName = pkg.packageName; process.packageCode = pkg;
            ((List<ActivityTaskManagerService.ProcessRecord>)get(atm, "mProcesses")).add(process);
        });
    }
    @SuppressWarnings("unchecked")
    private static void catalog(PackageManagerService pm, ApkPackage authority) throws Exception {
        Object catalog = get(pm.permissions(), "mCatalog");
        set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
        PermissionInfo install = new PermissionInfo(); install.name = Manifest.permission.INSTALL_PACKAGES;
        install.packageName = authority.packageName; install.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;
        ((Map<String, PermissionInfo>)get(catalog, "mFramework")).put(install.name, install);
    }
    private static PackageManager client(PackageManagerService pm) throws Exception {
        Object context = unsafe.allocateInstance(Class.forName("android.app.ContextImpl"));
        set(context, "mUser", UserHandle.of(0));
        PackageManager client = (PackageManager)unsafe.allocateInstance(
                Class.forName("android.app.ApplicationPackageManager"));
        set(client, "mContext", context); set(client, "mPM", pm); return client;
    }
    private static void source(ApkPackage target, ApkPackage initiator) {
        target.setInstallSource(PackageInstallSource.fromSession(initiator, PackageInstaller.PACKAGE_SOURCE_STORE));
    }
    private static void history(InstallSourceInfo info, String initiating, String installing,
            Signature[] signing, String why) {
        check(java.util.Objects.equals(initiating, info.getInitiatingPackageName())
                && java.util.Objects.equals(installing, info.getInstallingPackageName()), why + " names");
        check(initiating == null ? info.getInitiatingPackageSigningInfo() == null
                : Arrays.equals(signing, info.getInitiatingPackageSigningInfo().getApkContentsSigners()), why + " signer");
        check(info.getPackageSource() == PackageInstaller.PACKAGE_SOURCE_STORE
                && info.getOriginatingPackageName() == null && info.getUpdateOwnerPackageName() == null,
                why + " category and unimplemented identities");
    }
    private static void blockedOn(Thread thread, Object monitor) throws Exception {
        long end = System.nanoTime() + 5_000_000_000L;
        while (System.nanoTime() < end) {
            var info = ManagementFactory.getThreadMXBean().getThreadInfo(thread.threadId());
            if (info != null && info.getThreadState() == Thread.State.BLOCKED && info.getLockInfo() != null
                    && info.getLockInfo().getIdentityHashCode() == System.identityHashCode(monitor)) return;
            if (!thread.isAlive()) throw new AssertionError("Expected blocked operation already completed");
            Thread.yield();
        }
        throw new AssertionError("Operation did not reach the expected monitor");
    }
    private static void completed(Thread thread) throws Exception {
        thread.join(5000); check(!thread.isAlive(), "Bounded operation deadlocked");
    }
    private static Object registryEntry(String name) throws Exception {
        var read = Installer.class.getDeclaredMethod("readRegistry"); read.setAccessible(true);
        return ((Map<?, ?>)read.invoke(null)).get(name);
    }
    private static String[] unchanged(Installer storage, ApkPackage pkg,
            PackageInstallSource source) throws Exception {
        Object entry = ((Map<?, ?>)get(storage, "mEntries")).get(pkg.packageName);
        var method = Installer.class.getDeclaredMethod("unchanged", String.class, String.class,
                ApkPackage.class, entry.getClass(), PackageInstallSource.class);
        method.setAccessible(true);
        try {
            synchronized (storage) {
                return (String[])method.invoke(storage, pkg.apkPath, pkg.packageName, pkg, entry, source);
            }
        } catch (java.lang.reflect.InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception cause) throw cause;
            if (failure.getCause() instanceof Error cause) throw cause;
            throw failure;
        }
    }
    private static void unchangedCompilation(PackageManagerService pm, ActivityTaskManagerService atm,
            Installer storage, ApkPackage pkg, ApkPackage next) throws Exception {
        var sessions = pm.getPackageInstaller();
        var params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        int[] id = new int[1];
        as(pkg, () -> id[0] = sessions.createSession(params, pkg.packageName, null, 0));
        Path oat = Dexopt.oatDir(new File(pkg.apkPath).getParent()).toPath().resolve("base.odex");
        check(!Files.exists(oat), "Cache-rebuild control has no compiled output");
        PackageInstallSource original = pkg.installSource();
        String[] result = unchanged(storage, pkg, null);
        check("unchanged".equals(result[1]) && pm.get(pkg.packageName) == pkg,
                "Cache-only rebuild preserves the current installed code identity");
        check(pkg.installSource() == original, "Host reopen preserves original attribution");
        as(pkg, () -> {
            check(sessions.openSession(id[0]) != null, "Cache-only rebuild preserves existing session authority");
            int fresh = sessions.createSession(params, pkg.packageName, null, 0);
            check(sessions.openSession(fresh) != null, "Cache-only rebuild preserves fresh session authority");
            sessions.abandonSession(fresh);
        });
        PackageInstallSource updated = PackageInstallSource.fromSession(next,
                PackageInstaller.PACKAGE_SOURCE_LOCAL_FILE);
        unchanged(storage, pkg, updated);
        check(pm.get(pkg.packageName) == pkg && pkg.installSource() == updated,
                "Explicit same-code reinstall changes attribution without retiring code");
        check(updated.equals(get(registryEntry(pkg.packageName), "installSource")),
                "Same-code attribution is durable after cache-only rebuild");
        Path registry = data.resolve("system/packages.list");
        String stable = Files.readString(registry);
        Path blocked = registry.resolveSibling("packages.list.tmp"); Files.createDirectory(blocked);
        try {
            expect(java.io.IOException.class, () -> unchanged(storage, pkg, original),
                    "Same-code attribution reports registry failure during cache rebuild");
            check(pm.get(pkg.packageName) == pkg && pkg.installSource() == updated
                    && Files.readString(registry).equals(stable),
                    "Failed same-code attribution retains current identity, memory and durable source");
            as(pkg, () -> check(sessions.openSession(id[0]) != null,
                    "Failed attribution persistence does not retire existing session authority"));
        } finally { Files.delete(blocked); }
        unchanged(storage, pkg, original);
        as(pkg, () -> sessions.abandonSession(id[0]));
    }
    private static void publicationRollback(PackageManagerService pm, Installer storage,
            ApkPackage current, ApkPackage installer) throws Exception {
        ApkPackage replacement = pkg(current.packageName, "v2only.apk", null, 36);
        replacement.assignUid(-1);
        PackageInstallSource source = PackageInstallSource.fromSession(installer,
                PackageInstaller.PACKAGE_SOURCE_LOCAL_FILE);
        replacement.setInstallSource(source);
        Object previousEntry = ((Map<?, ?>)get(storage, "mEntries")).get(current.packageName);
        Class<?> type = previousEntry.getClass();
        var constructor = type.getDeclaredConstructor(String.class, String.class, long.class,
                long.class, PackageInstallSource.class);
        constructor.setAccessible(true);
        Object replacementEntry = constructor.newInstance(current.packageName,
                new File(replacement.apkPath).getParent(), 1L, 2L, source);
        var publish = Installer.class.getDeclaredMethod("publishInstalled", type, ApkPackage.class);
        publish.setAccessible(true);
        Path registry = data.resolve("system/packages.list");
        String before = Files.readString(registry);
        Path uids = data.resolve("system/package-uids");
        java.util.Properties beforeUids = new java.util.Properties();
        try (var input = Files.newInputStream(uids)) { beforeUids.load(input); }
        Path blocked = registry.resolveSibling("packages.list.tmp"); Files.createDirectory(blocked);
        try {
            expect(java.io.IOException.class, () -> {
                synchronized (storage) {
                    try { publish.invoke(storage, replacementEntry, replacement); }
                    catch (java.lang.reflect.InvocationTargetException failure) {
                        if (failure.getCause() instanceof Exception cause) throw cause;
                        throw failure;
                    }
                }
            }, "Failed package registry publication reports the durable write failure");
            java.util.Properties afterUids = new java.util.Properties();
            try (var input = Files.newInputStream(uids)) { afterUids.load(input); }
            check(beforeUids.equals(afterUids) && replacement.uidPending() && replacement.uid() == -1,
                    "Failed attributed publication rolls back the admitted UID and signer membership");
            check(pm.get(current.packageName) == current
                    && ((Map<?, ?>)get(storage, "mEntries")).get(current.packageName) == previousEntry
                    && Files.readString(registry).equals(before),
                    "Failed attributed publication preserves package, registry entry and durable attribution");
        } finally { Files.delete(blocked); }
    }
    private static void sessionIdentity(PackageManagerService pm, ActivityTaskManagerService atm,
            Installer storage) throws Exception {
        var sessions = pm.getPackageInstaller();
        String name = "fixture.source.session";
        ApkPackage original = pkg(name, "v2only.apk", null, 36);
        pm.add(original); attach(atm, original);
        String originalOwner = original.enabledStateOwner();
        var params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setPackageSource(PackageInstaller.PACKAGE_SOURCE_STORE);
        int[] id = new int[1];
        as(original, () -> id[0] = sessions.createSession(params, name, null, 0));
        as(original, () -> check(sessions.openSession(id[0]) != null,
                "Current authenticated installer owns its session"));
        as(original, () -> {
            check(sessions.getAllSessions(0).getList().size() == 1,
                    "Current installer can enumerate its session");
            check(sessions.getMySessions(name, 0).getList().size() == 1,
                    "Current installer retains its own session inventory");
        });
        @SuppressWarnings("unchecked") List<ActivityTaskManagerService.ProcessRecord> attached =
                (List<ActivityTaskManagerService.ProcessRecord>)get(atm, "mProcesses");
        ActivityTaskManagerService.ProcessRecord process = attached.stream()
                .filter(candidate -> candidate.pid == pids.get(original)).findFirst().orElseThrow();
        process.exiting = true;
        as(original, () -> {
            check(sessions.openSession(id[0]) != null, "Live exiting installer retains its session");
            int fresh = sessions.createSession(params, name, null, 0);
            check(fresh != id[0], "Live exiting installer may initiate a session before actual death");
            sessions.abandonSession(fresh);
        });
        process.dead = true;
        as(original, () -> expect(SecurityException.class,
                () -> sessions.createSession(params, name, null, 0), "Dead installer cannot create sessions"));
        as(original, () -> expect(SecurityException.class,
                () -> sessions.openSession(id[0]), "Dead installer cannot reopen sessions"));
        process.dead = false;
        as(original, () -> sessions.abandonSession(id[0]));

        // The old process can survive the bounded force-stop wait. A same-name reinstall
        // gets a new owner and may have a different signer, without lending either to it.
        pm.remove(name);
        ApkPackage replacement = pkg(name, "rotated.apk", null, 36);
        pm.add(replacement); attach(atm, replacement);
        check(!originalOwner.equals(replacement.enabledStateOwner())
                && !Arrays.equals(original.verifiedSignatures(), replacement.verifiedSignatures()),
                "Replacement control has a new installation owner and current signer");
        as(replacement, () -> id[0] = sessions.createSession(params, name, null, 0));
        as(original, () -> expect(SecurityException.class,
                () -> sessions.createSession(params, name, null, 0),
                "Superseded attached code cannot create a session as its replacement"));
        as(original, () -> expect(SecurityException.class,
                () -> sessions.openSession(id[0]), "Superseded attached code cannot open its replacement's session"));
        as(replacement, () -> {
            check(sessions.openSession(id[0]) != null, "Replacement installer retains its own session authority");
            sessions.abandonSession(id[0]);
        });
    }
    private static void sharedSetter(PackageManagerService pm, ActivityTaskManagerService atm)
            throws Exception {
        String group = "fixture.source.settergroup";
        ApkPackage target = pkg("fixture.source.settertarget", "v2only.apk", null, 36);
        ApkPackage installer = pkg("fixture.source.setterinstaller", "v2only.apk", null, 36);
        ApkPackage modern = pkg("fixture.source.settermodern", "v2only.apk", group, 36,
                target.packageName, installer.packageName);
        ApkPackage legacy = pkg("fixture.source.setterlegacy", "v2only.apk", group, 29);
        modern.assignUid(-1); legacy.assignUid(-1);
        pm.add(target); pm.add(installer); pm.add(modern); pm.add(legacy);
        attach(atm, modern);
        PackageManager client = client(pm);
        as(modern, () -> client.setInstallerPackageName(target.packageName, installer.packageName));
        check(target.installSource() == PackageInstallSource.UNKNOWN,
                "Shared-user compatibility uses every member and still denies a missing permission");
        pm.beginPackageRemoval(legacy);
        as(modern, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(target.packageName, installer.packageName),
                "Retiring legacy peer cannot change the surviving modern caller's denial"));
        pm.remove(legacy.packageName); pm.endPackageRemoval(legacy);
        as(modern, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(target.packageName, installer.packageName),
                "Removed legacy peer cannot change the surviving modern caller's denial"));

        String rotationGroup = "fixture.source.rotationgroup";
        ApkPackage older = pkg("fixture.source.groupolder", "../identity-fixtures/old.apk", rotationGroup, 36,
                target.packageName, "fixture.source.groupnewer");
        ApkPackage newer = pkg("fixture.source.groupnewer", "../identity-fixtures/old.apk", rotationGroup, 36);
        older.assignUid(-1); newer.assignUid(-1); pm.add(older); pm.add(newer); attach(atm, older);
        ApkPackage rotated = pkg(newer.packageName, "../identity-fixtures/rotated-no-auth.apk", rotationGroup, 36);
        rotated.assignUid(-1);
        try (PackageUids.Admission failed = pm.prepareAdd(rotated)) {
            failed.commit(); // Simulate failure before the installed registry publishes this rotation.
        }
        check(rotated.uidPending() && Arrays.equals(PackageUids.sharedSigningDetails(older).getSignatures(),
                older.verifiedSignatures()), "Unpublished rotation rolls back the shared signing snapshot");
        pm.add(rotated);
        check(older.uid() == rotated.uid()
                && !Arrays.equals(older.verifiedSignatures(), rotated.verifiedSignatures()),
                "Production shared-user admission accepts the verified signing rotation");
        source(target, rotated);
        as(older, () -> client.setInstallerPackageName(target.packageName, rotated.packageName));
        check(target.installSource().isInstaller(rotated),
                "Shared-user setter compares the group's current signer after a peer rotates");
        as(older, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(target.packageName, older.packageName),
                "A group's old certificate is not equal to its current installer certificate"));
        // Exactly the previous registry layout: active package signers, no shared snapshot.
        try (PackageUids.Admission legacyRegistry = new PackageUids.Admission()) {
            legacyRegistry.after.remove("signer:shared:" + rotationGroup);
            legacyRegistry.after.remove("lineage:shared:" + rotationGroup);
            legacyRegistry.commit(); legacyRegistry.accept();
        }
        check(Arrays.equals(PackageUids.sharedSigningDetails(older).getSignatures(),
                rotated.verifiedSignatures()), "Legacy identity registry derives the current verified group signer");
        source(target, older);
        pm.remove(rotated.packageName);
        as(older, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(target.packageName, older.packageName),
                "Removing rotated peer must not roll shared-user signer back to old certificate"));
        try (PackageUids.Admission failed = PackageUids.prepareRetire(older)) {
            failed.commit(); // Last-member deletion must roll back with an uncommitted uninstall.
        }
        check(Arrays.equals(PackageUids.sharedSigningDetails(older).getSignatures(),
                rotated.verifiedSignatures()), "Failed final retirement preserves the shared signing snapshot");
        ApkPackage reopened = pkg(older.packageName, "../identity-fixtures/old.apk", rotationGroup, 36);
        reopened.assignUid(-1);
        new PackageManagerService(List.of(reopened));
        check(Arrays.equals(PackageUids.sharedSigningDetails(reopened).getSignatures(),
                rotated.verifiedSignatures()), "Restart admission cannot roll back the retained group signer");
        pm.remove(older.packageName);
        expect(SecurityException.class, () -> PackageUids.sharedSigningDetails(older),
                "A retired last member has no remaining shared signing authority");
        ApkPackage reset = pkg("fixture.source.groupreset", "v2only.apk", rotationGroup, 36);
        reset.assignUid(-1); pm.add(reset);
        check(Arrays.equals(PackageUids.sharedSigningDetails(reset).getSignatures(), reset.verifiedSignatures()),
                "A later group lifetime starts from its new verified signer after final retirement");

        String capabilityGroup = "fixture.source.capabilitygroup";
        ApkPackage noAuth = pkg("fixture.source.capabilitymember", "../identity-fixtures/rotated-no-auth.apk",
                capabilityGroup, 36);
        noAuth.assignUid(-1); pm.add(noAuth);
        check(!PackageUids.sharedSigningDetails(noAuth).checkCapability(older.verifiedSigningDetails(),
                SigningDetails.CertCapabilities.AUTH), "Initial shared lineage honors revoked ancestor capability");
        ApkPackage revokingPeer = pkg("fixture.source.capabilitypeer", "../identity-fixtures/rotated-no-auth.apk",
                capabilityGroup, 36);
        revokingPeer.assignUid(-1); pm.add(revokingPeer);
        ApkPackage noData = pkg(noAuth.packageName, "../identity-fixtures/rotated-no-data.apk", capabilityGroup, 36);
        noData.assignUid(-1); pm.add(noData);
        check(!PackageUids.sharedSigningDetails(noData).checkCapability(older.verifiedSigningDetails(),
                SigningDetails.CertCapabilities.AUTH), "Another current member retains its ancestor capability veto");
        pm.remove(revokingPeer.packageName);
        check(!PackageUids.sharedSigningDetails(noData).checkCapability(older.verifiedSigningDetails(),
                SigningDetails.CertCapabilities.AUTH), "Removal alone does not rewrite shared lineage capabilities");
        ApkPackage refreshedMember = pkg(noData.packageName, "../identity-fixtures/rotated-no-data.apk", capabilityGroup, 36);
        refreshedMember.assignUid(-1); pm.add(refreshedMember);
        SigningDetails refreshed = PackageUids.sharedSigningDetails(refreshedMember);
        check(refreshed.checkCapability(older.verifiedSigningDetails(), SigningDetails.CertCapabilities.AUTH)
                && !refreshed.checkCapability(older.verifiedSigningDetails(), SigningDetails.CertCapabilities.INSTALLED_DATA),
                "Current signed update refreshes ancestor capabilities after the last revoking peer is gone");
    }
    private static void sharedHistory(PackageManagerService pm, ActivityTaskManagerService atm)
            throws Exception {
        ApkPackage initiator = pkg("fixture.source.sharedinstaller", "v2only.apk", null, 36);
        ApkPackage target = pkg("fixture.source.sharedtarget", "v2only.apk", "fixture.source.group", 36);
        ApkPackage peer = pkg("fixture.source.sharedpeer", "v2only.apk", "fixture.source.group", 36);
        // Use the production admission transaction, including full signature verification;
        // a coincident numeric UID alone must still fail the existing controls above.
        target.assignUid(-1); peer.assignUid(-1);
        pm.add(initiator); pm.add(target); pm.add(peer);
        check(target.uid() == peer.uid() && target.uid() >= android.os.Process.FIRST_APPLICATION_UID,
                "Verified shared-user members were not admitted to the same application UID");
        attach(atm, target); attach(atm, peer);
        source(target, initiator);
        Signature[] signing = initiator.verifiedSignatures();
        pm.remove(initiator.packageName);
        PackageManager client = client(pm);
        as(peer, () -> history(client.getInstallSourceInfo(target.packageName), initiator.packageName,
                null, signing, "Authenticated shared-user peer retains historical initiator"));

        pm.beginPackageRemoval(peer);
        as(peer, () -> history(client.getInstallSourceInfo(target.packageName), null, null, signing,
                "Retiring shared-user peer cannot disclose historical initiator"));
        pm.remove(peer.packageName); pm.endPackageRemoval(peer);
        ApkPackage replacement = pkg(peer.packageName, "v2only.apk", "fixture.source.group", 36);
        replacement.assignUid(-1); pm.add(replacement);
        as(peer, () -> history(client.getInstallSourceInfo(target.packageName), null, null, signing,
                "Old shared-user process cannot adopt replacement installation authority"));
        attach(atm, replacement);
        as(replacement, () -> history(client.getInstallSourceInfo(target.packageName), initiator.packageName,
                null, signing, "New authenticated shared-user peer retains group visibility"));
    }
    private static void run() throws Exception {
        String targetName = "fixture.source.target", oldName = "fixture.source.original";
        String nextName = "fixture.source.next", rotatedName = "fixture.source.rotated";
        String clearName = "fixture.source.unknown";
        ApkPackage target = pkg(targetName, "v2only.apk", null, 36);
        ApkPackage old = pkg(oldName, "v2only.apk", null, 36, targetName, nextName, clearName, rotatedName, "fixture.source.cli", "fixture.source.bundled");
        ApkPackage next = pkg(nextName, "v2only.apk", null, 36, targetName, oldName, clearName);
        ApkPackage rotated = pkg(rotatedName, "rotated.apk", null, 36, targetName, oldName, nextName);
        ApkPackage unknown = pkg(clearName, "v2only.apk", null, 36);
        ApkPackage legacy = pkg("fixture.source.legacy", "v2only.apk", null, 29, clearName, nextName);
        ApkPackage unsigned = pkg("fixture.source.unsigned", "unsigned.apk", null, 36, targetName, nextName);
        ApkPackage observer = pkg("fixture.source.observer", "v2only.apk", null, 36, targetName, nextName);
        ApkPackage initObserver = pkg("fixture.source.initobserver", "v2only.apk", null, 36, targetName, oldName);
        ApkPackage authority = pkg("fixture.source.authority", "v2only.apk", null, 36);
        ApkPackage privileged = pkg("fixture.source.privileged", "v2only.apk", null, 36, clearName, nextName, targetName);
        ApkPackage cli = pkg("fixture.source.cli", "v2only.apk", null, 36);
        ApkPackage bundled = pkg("fixture.source.bundled", "v2only.apk", null, 36);
        // Metadata does not rely on ordinary installability or addon privileges.
        bundled.setSystemAddon(true);
        List<ApkPackage> packages = new ArrayList<>(List.of(target, old, next, rotated,
                unknown, legacy, unsigned, observer, initObserver, authority, privileged, cli, bundled));
        PackageManagerService pm = new PackageManagerService(packages);
        catalog(pm, authority);
        ActivityTaskManagerService atm = new ActivityTaskManagerService(pm);
        pm.attachInstallSourceCallers(atm);
        for (ApkPackage pkg : packages) attach(atm, pkg);
        check(target.uid() != old.uid(), "Production UID allocation isolates unrelated packages in both profiles");
        Signature[] original = old.verifiedSignatures();
        check(original != null && rotated.verifiedSigningDetails().getPastSigningCertificates().length == 2,
                "Real v2 and rotated APK verification");
        check(unsigned.verifiedSigningDetails() == SigningDetails.UNKNOWN, "Unsigned fixture is not a signing identity");
        Path code = data.resolve("app/" + targetName + "-1"); Files.createDirectories(code);
        Files.copy(fixtures.resolve("v2only.apk"), code.resolve("base.apk"));
        set(target, "apkPath", code.resolve("base.apk").toFile().getCanonicalPath());
        source(target, old); source(cli, old); source(bundled, old);
        Path registry = data.resolve("system/packages.list");
        Files.writeString(registry, targetName + "\t" + code + "\t1\t2\t" + target.installSource().encode()
                + "\nfixture.legacy.row\t/unchanged/legacy\t3\t4\n");
        Installer.attachMetadata(pm, atm);
        Installer storage = (Installer)get(pm, "mInstallSourceStorage");
        pm.attachInstaller(atm, storage);
        unchangedCompilation(pm, atm, storage, target, next);
        publicationRollback(pm, storage, target, next);
        PackageManager client = client(pm);
        as(old, () -> client.setInstallerPackageName(targetName, nextName));
        as(target, () -> {
            history(client.getInstallSourceInfo(targetName), oldName, nextName, original, "Reassignment preserves history");
            check(client.canPackageQuery(targetName, oldName) && client.canPackageQuery(targetName, nextName),
                    "Both installed initiator and current installer remain visible");
        });
        as(observer, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Observer can see installer independently"));
        as(initObserver, () -> history(client.getInstallSourceInfo(targetName), oldName, null, original,
                "Observer can see initiator independently"));
        check(((PackageInstallSource)get(registryEntry(targetName), "installSource")).installer().equals(nextName),
                "Actual ordinary registry reopens reassigned installer");
        check(Files.readString(registry).contains("fixture.legacy.row\t/unchanged/legacy\t3\t4\n"),
                "Legacy four-column row preserved without fake attribution");
        as(unsigned, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, nextName), "Unsigned caller cannot match installer"));
        as(rotated, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, rotatedName), "Lineage capability is not current certificate equality"));
        as(old, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, rotatedName), "New installer must match current caller cert"));
        as(observer, () -> expect(IllegalArgumentException.class,
                () -> client.setInstallerPackageName("fixture.hidden", nextName), "Unknown target rejected"));
        as(old, () -> expect(IllegalArgumentException.class,
                () -> client.setInstallerPackageName(targetName, "fixture.hidden"), "Unknown installer rejected"));
        as(observer, () -> expect(IllegalArgumentException.class,
                () -> client.setInstallerPackageName(targetName, oldName), "Installed hidden new installer rejected"));
        as(old, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(clearName, nextName), "Modern caller needs INSTALL_PACKAGES for unattributed target"));
        as(legacy, () -> client.setInstallerPackageName(clearName, nextName));
        check(unknown.installSource() == PackageInstallSource.UNKNOWN, "Legacy missing-permission denial remains silent and unchanged");
        unknown.setInstallSource(PackageInstallSource.fromSession(null, PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE));
        as(privileged, () -> client.setInstallerPackageName(clearName, nextName));
        check(unknown.installSource().installer().equals(nextName) && unknown.installSource().initiator() == null
                && unknown.installSource().category() == PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE,
                "Real signature-permission policy permits reassignment without inventing a historical initiator");
        as(next, () -> client.setInstallerPackageName(clearName, null));
        check(unknown.installSource().installer() == null, "Same signer may clear existing installer without INSTALL_PACKAGES");
        as(next, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(clearName, null), "Even an unattributed clear obeys compatibility permission rule"));

        // A legacy numeric UID collision does not make another package's target SDK
        // or certificates authoritative. The modern caller remains denied above.
        check(old.parsed.getTargetSdkVersion() >= 30 && legacy.parsed.getTargetSdkVersion() < 30,
                "SDK compatibility controls use distinct authenticated packages");

        String stable = Files.readString(registry); PackageInstallSource before = target.installSource();
        Path blocked = registry.resolveSibling("packages.list.tmp"); Files.createDirectory(blocked);
        as(old, () -> expect(IllegalStateException.class,
                () -> client.setInstallerPackageName(targetName, oldName), "Durable-write failure reported"));
        check(target.installSource() == before && Files.readString(registry).equals(stable), "Failure rolls back source and durable registry");
        Files.delete(blocked);
        as(old, () -> client.setInstallerPackageName(cli.packageName, nextName));
        as(old, () -> client.setInstallerPackageName(bundled.packageName, nextName));
        check(registryEntry(cli.packageName) == null && registryEntry(bundled.packageName) == null,
                "CLI and bundled metadata create no fake installed-code entries");
        check(Files.readString(registry).contains("# install-source\t" + cli.packageName + "\t"), "Metadata comment row persisted");
        ApkPackage reopenedCli = pkg(cli.packageName, "v2only.apk", null, 36);
        ApkPackage reopenedBundled = pkg(bundled.packageName, "v2only.apk", null, 36);
        PackageManagerService restored = new PackageManagerService(List.of(reopenedCli, reopenedBundled, old, next));
        Installer.attachMetadata(restored, new ActivityTaskManagerService(restored));
        check(reopenedCli.installSource().installer().equals(nextName)
                && reopenedBundled.installSource().initiator().equals(oldName), "Metadata restored against persistent installation owners");
        set(reopenedCli, "mEnabledOwner", java.util.UUID.randomUUID().toString());
        reopenedCli.setInstallSource(PackageInstallSource.UNKNOWN);
        Object restoredStorage = get(restored, "mInstallSourceStorage");
        var restore = Installer.class.getDeclaredMethod("restoreInstallSources"); restore.setAccessible(true); restore.invoke(restoredStorage);
        check(reopenedCli.installSource() == PackageInstallSource.UNKNOWN, "Different target installation owner cannot inherit metadata");

        // A real removal retires the initiating owner, while the independent current installer lives.
        pm.remove(oldName);
        as(target, () -> history(client.getInstallSourceInfo(targetName), oldName, nextName, original,
                "Authenticated target retains retired history"));
        @SuppressWarnings("unchecked") List<ActivityTaskManagerService.ProcessRecord> attached =
                (List<ActivityTaskManagerService.ProcessRecord>)get(atm, "mProcesses");
        ActivityTaskManagerService.ProcessRecord exitingTarget = attached.stream()
                .filter(process -> process.pid == pids.get(target)).findFirst().orElseThrow();
        exitingTarget.exiting = true;
        try {
            as(target, () -> history(client.getInstallSourceInfo(targetName), oldName, nextName, original,
                    "Live exiting target retains authenticated historical initiator until death"));
            exitingTarget.dead = true;
            as(target, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                    "Dead attached process cannot retain historical-query authority"));
        } finally {
            exitingTarget.dead = false;
            exitingTarget.exiting = false;
        }
        // The authenticated target needs no all-package graph construction.
        @SuppressWarnings("unchecked") Map<String, ApkPackage> actual = (Map<String, ApkPackage>)get(pm, "mPackages");
        Map<String, ApkPackage> noIteration = new java.util.AbstractMap<>() {
            @Override public ApkPackage get(Object key) { return actual.get(key); }
            @Override public Set<Map.Entry<String, ApkPackage>> entrySet() {
                throw new AssertionError("Self install-source query traversed the package graph");
            }
        };
        set(pm, "mPackages", noIteration);
        try {
            as(target, () -> check(client.getInstallSourceInfo(targetName).getInitiatingPackageName().equals(oldName), "Self getter avoids package-graph traversal"));
        } finally { set(pm, "mPackages", actual); }

        as(observer, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Third-party observer cannot see retired history"));
        history(pm.getInstallSourceInfo(targetName, 0), null, nextName, original,
                "Core visibility alone does not disclose retired initiator");
        as(999999, target.uid(), () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Unauthenticated matching numeric UID does not disclose retired history"));
        as(999999, next.uid(), () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, nextName), "Unknown PID cannot change installer"));
        int observerUid = observer.uid(); set(observer, "mUid", target.uid());
        as(observer, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Legacy numeric UID collision is not a shared-user peer"));
        set(observer, "mUid", observerUid);
        ApkPackage reincarnated = pkg(oldName, "v2only.apk", null, 36); pm.add(reincarnated);
        check(!target.installSource().isInitiator(reincarnated), "Same-name new installation cannot revive initiator visibility");
        as(target, () -> expect(PackageManager.NameNotFoundException.class,
                () -> client.canPackageQuery(targetName, oldName), "Retired initiator visibility edge stays retired"));
        as(target, () -> history(client.getInstallSourceInfo(targetName), oldName, nextName, original,
                "Target history survives a different same-name installation"));

        // Revalidate the exact caller after authentication while a setter waits on Installer.
        AtomicReference<Throwable> setterFailure = new AtomicReference<>();
        Thread setter = new Thread(() -> { try { as(next, () -> client.setInstallerPackageName(targetName, null)); }
            catch (Throwable failure) { setterFailure.set(failure); } }, "installer-caller-race");
        synchronized (storage) {
            setter.start(); blockedOn(setter, storage);
            ApkPackage replacement = pkg(nextName, "v2only.apk", null, 36); pm.add(replacement);
        }
        completed(setter); check(setterFailure.get() instanceof SecurityException, "Replaced setter caller loses captured authority");
        check(target.installSource().installer().equals(nextName), "Rejected racing setter leaves metadata unchanged");
        as(next, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, null),
                "Superseded attached code cannot adopt a replacement by looking up its name"));

        // Revalidate the target after ATM authentication and before reading retired history.
        AtomicReference<InstallSourceInfo> result = new AtomicReference<>();
        AtomicReference<Throwable> getterFailure = new AtomicReference<>();
        Thread getter = new Thread(() -> { try { as(target, () -> result.set(client.getInstallSourceInfo(targetName))); }
            catch (Throwable failure) { getterFailure.set(failure); } }, "installer-history-race");
        synchronized (pm) {
            getter.start(); blockedOn(getter, pm);
            pm.add(pkg(targetName, "v2only.apk", null, 36));
        }
        completed(getter);
        check(getterFailure.get() == null && result.get().getInitiatingPackageName() == null,
                "Replaced historical getter code is rejected within coherent PM snapshot");

        // Versioned and original formats, category-only rows and unknown signing are round-tripped.
        PackageInstallSource separated = target.installSource();
        PackageInstallSource decoded = PackageInstallSource.decode(separated.encode());
        check(decoded.initiator().equals(oldName) && decoded.installer().equals(nextName)
                && Arrays.equals(decoded.signingDetails().getSignatures(), original), "Independent roles round-trip with certificates");
        PackageInstallSource cleared = separated.withInstaller(null);
        check(PackageInstallSource.decode(cleared.encode()).installer() == null
                && PackageInstallSource.decode(cleared.encode()).initiator().equals(oldName), "Clear retains historical identity on disk");
        PackageInstallSource category = PackageInstallSource.fromSession(null, PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE);
        check(PackageInstallSource.decode(category.encode()).category() == category.category()
                && category.installer() == null, "Contributor category-without-identity behavior survives");
        check(new PackageInstallSource(null, null, 0, SigningDetails.UNKNOWN).signingInfo() == null,
                "Unknown verified signing details are normalized");
        expect(IllegalArgumentException.class, () -> PackageInstallSource.decode("v2,a,b,c,d,2,,,"), "Malformed UUIDs rejected");
        expect(IllegalArgumentException.class, () -> PackageInstallSource.decode(
                "v2,,," + nextName + "," + next.enabledStateOwner() + ",99,,,"), "Malformed category rejected");
        expect(IllegalArgumentException.class, () -> PackageInstallSource.decode(
                nextName + "," + next.enabledStateOwner() + ",2,,,"), "Malformed legacy signer block remains invalid");
        check(PackageInstallSource.decode("") == PackageInstallSource.UNKNOWN, "Old missing source is unknown");
        check(PackageInstallSource.decode(nextName + "," + next.enabledStateOwner() + ",2").initiator().equals(nextName),
                "Original three-field identity maps to both roles");
        sessionIdentity(pm, atm, storage);
        sharedSetter(pm, atm);
        sharedHistory(pm, atm);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); fixtures = Path.of(args[1]); data = Path.of(System.getenv("ANDROID_DATA"));
        Files.createDirectories(data);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class, "sPackageInfoCache").get(null)).testPropertyName();
        run();
        System.out.println("InstallSourceHost passed: " + checks + " checks; actual APK verification, PMS and registry; constructed manifests/catalog and native host boundaries, no Windows/ART or Binder transport claim.");
    }
}
