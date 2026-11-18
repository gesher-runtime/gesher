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

/** Actual PMS, UID admission, APK verifier and registry; constructed manifests and host JNI only. */
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
        set(result, "mUid", -1); set(result, "mUidPending", true);
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
    private static void run() throws Exception {
        String targetName = "fixture.source.target", oldName = "fixture.source.original";
        String nextName = "fixture.source.next", rotatedName = "fixture.source.rotated";
        String clearName = "fixture.source.unknown";
        ApkPackage target = pkg(targetName, "v2only.apk", "fixture.target.group", 36);
        ApkPackage targetPeer = pkg("fixture.source.targetpeer", "v2only.apk", "fixture.target.group", 36);
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
        List<ApkPackage> packages = new ArrayList<>(List.of(target, targetPeer, old, next, rotated,
                unknown, legacy, unsigned, observer, initObserver, authority, privileged, cli, bundled));
        PackageManagerService pm = new PackageManagerService(packages);
        catalog(pm, authority);
        ActivityTaskManagerService atm = new ActivityTaskManagerService(pm);
        pm.attachInstallSourceCallers(atm);
        for (ApkPackage pkg : packages) attach(atm, pkg);
        check(target.uid() == targetPeer.uid() && old.uid() != target.uid(), "Real UID admission formed only declared shared group");
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
        PackageManager client = client(pm);
        as(old, () -> client.setInstallerPackageName(targetName, nextName));
        as(target, () -> {
            history(client.getInstallSourceInfo(targetName), oldName, nextName, original, "Reassignment preserves history");
            check(client.canPackageQuery(targetName, oldName) && client.canPackageQuery(targetName, nextName),
                    "Both installed initiator and current installer remain visible");
        });
        as(targetPeer, () -> history(client.getInstallSourceInfo(targetName), oldName, nextName, original,
                "Admitted UID peer sees both current relationships"));
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

        // Genuine shared users use the group's current signer (a verified merged lineage),
        // and compatibility-by-UID is disabled when any admitted member still targets Q.
        String groupTargetName = "fixture.source.grouptarget", groupNextName = "fixture.source.groupnext";
        ApkPackage groupCaller = pkg("fixture.source.groupcaller", "v2only.apk", "fixture.signer.group", 36,
                groupTargetName, groupNextName, clearName);
        ApkPackage groupPeer = pkg("fixture.source.grouppeer", "rotated.apk", "fixture.signer.group", 29,
                groupTargetName, groupNextName, clearName);
        ApkPackage groupTarget = pkg(groupTargetName, "v2only.apk", null, 36);
        ApkPackage groupNext = pkg(groupNextName, "rotated.apk", null, 36);
        for (ApkPackage member : List.of(groupCaller, groupPeer, groupTarget, groupNext)) {
            pm.add(member); attach(atm, member);
        }
        check(groupCaller.uid() == groupPeer.uid()
                && !Arrays.equals(groupCaller.verifiedSignatures(), groupNext.verifiedSignatures()),
                "Admitted rotation fixture distinguishes individual and shared current signer");
        source(groupTarget, rotated);
        as(groupCaller, () -> client.setInstallerPackageName(groupTargetName, groupNextName));
        check(groupTarget.installSource().installer().equals(groupNextName), "Shared-user current signer authorizes matching new and old installers");
        as(groupCaller, () -> client.setInstallerPackageName(clearName, groupNextName));
        check(unknown.installSource().installer() == null, "Lowest shared UID SDK silently denies missing INSTALL_PACKAGES");
        ApkPackage modernPeer = pkg(groupPeer.packageName, "rotated.apk", "fixture.signer.group", 36);
        pm.add(modernPeer); attach(atm, modernPeer);
        as(groupCaller, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(clearName, groupNextName), "All modern shared UID members receive permission exception"));
        int legacyUid = legacy.uid(); legacy.assignUid(groupCaller.uid());
        as(groupCaller, () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(clearName, groupNextName), "Unrelated legacy numeric UID does not disable shared-user compatibility"));
        legacy.assignUid(legacyUid);

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
        as(targetPeer, () -> history(client.getInstallSourceInfo(targetName), oldName, nextName, original,
                "Authenticated admitted shared UID peer retains retired history"));
        // The authenticated target and admitted peer need no all-package graph construction.
        @SuppressWarnings("unchecked") Map<String, ApkPackage> actual = (Map<String, ApkPackage>)get(pm, "mPackages");
        Map<String, ApkPackage> noIteration = new java.util.AbstractMap<>() {
            @Override public ApkPackage get(Object key) { return actual.get(key); }
            @Override public Set<Map.Entry<String, ApkPackage>> entrySet() {
                throw new AssertionError("Self install-source query traversed the package graph");
            }
        };
        set(pm, "mPackages", noIteration);
        try {
            as(target, () -> check(client.getInstallSourceInfo(targetName).getInitiatingPackageName().equals(oldName), "Self getter stays O(1)"));
            as(targetPeer, () -> check(client.getInstallSourceInfo(targetName).getInitiatingPackageName().equals(oldName), "Admitted shared UID getter stays O(1)"));
        } finally { set(pm, "mPackages", actual); }

        as(observer, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Third-party observer cannot see retired history"));
        history(pm.getInstallSourceInfo(targetName, 0), null, nextName, original,
                "Core visibility alone does not disclose retired initiator");
        as(999999, target.uid(), () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Unauthenticated matching numeric UID does not disclose retired history"));
        as(999999, next.uid(), () -> expect(SecurityException.class,
                () -> client.setInstallerPackageName(targetName, nextName), "Unknown PID cannot change installer"));
        int observerUid = observer.uid(); observer.assignUid(target.uid());
        as(observer, () -> history(client.getInstallSourceInfo(targetName), null, nextName, original,
                "Legacy numeric UID collision is not a shared-user peer"));
        observer.assignUid(observerUid);
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

        // Revalidate a genuine peer after ATM authentication and before reading retired history.
        AtomicReference<InstallSourceInfo> result = new AtomicReference<>();
        AtomicReference<Throwable> getterFailure = new AtomicReference<>();
        Thread getter = new Thread(() -> { try { as(targetPeer, () -> result.set(client.getInstallSourceInfo(targetName))); }
            catch (Throwable failure) { getterFailure.set(failure); } }, "installer-history-race");
        synchronized (pm) {
            getter.start(); blockedOn(getter, pm);
            pm.add(pkg(targetPeer.packageName, "v2only.apk", "fixture.target.group", 36));
        }
        completed(getter);
        check(getterFailure.get() == null && result.get().getInitiatingPackageName() == null,
                "Replaced historical getter peer is rejected within coherent PM snapshot");

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
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); fixtures = Path.of(args[1]); data = Path.of(System.getenv("ANDROID_DATA"));
        Files.createDirectories(data);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class, "sPackageInfoCache").get(null)).testPropertyName();
        run();
        System.out.println("InstallSourceHost passed: " + checks + " checks; actual APK verification, UID admission, PMS and registry; constructed manifests/catalog and native host boundaries, no Windows/ART or Binder transport claim.");
    }
}
