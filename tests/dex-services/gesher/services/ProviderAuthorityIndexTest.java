/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.os.Bundle;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedProvider;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Real PMS, provider resolver and Android metadata generation; manifests are fixtures. */
public final class ProviderAuthorityIndexTest {
    private static int checks;
    private static final AtomicInteger providerReads = new AtomicInteger();

    private static final class CountingPackage extends PackageImpl {
        CountingPackage(String name) {
            super(name, "/fixture/" + name + "/base.apk", "/fixture/" + name + "/base.apk", null, false, null);
        }
        @Override public List<ParsedProvider> getProviders() {
            providerReads.incrementAndGet(); return super.getProviders();
        }
    }

    private static void require(boolean value, String reason) {
        checks++;
        if (!value) throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        Method result = type.getDeclaredMethod(name, types); result.setAccessible(true); return result;
    }
    private static Unsafe unsafe() throws Exception {
        return (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
    }
    private static Object invoke(Method method, Object receiver, Object... args) throws Exception {
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            if (cause instanceof Error) throw (Error)cause;
            throw failure;
        }
    }
    private static ParsedProviderImpl provider(PackageImpl parsed, String name, String authority,
            boolean enabled, boolean aware) {
        ParsedProviderImpl result = new ParsedProviderImpl();
        result.setName(parsed.getPackageName() + "." + name);
        result.setPackageName(parsed.getPackageName());
        result.setProcessName(parsed.getPackageName());
        result.setAuthority(authority); result.setEnabled(enabled); result.setDirectBootAware(aware);
        result.setReadPermission(parsed.getPackageName() + ".READ");
        result.setWritePermission(parsed.getPackageName() + ".WRITE");
        Bundle metadata = new Bundle(); metadata.putString("owner", parsed.getPackageName());
        result.setMetaData(metadata); parsed.addProvider(result); return result;
    }
    private static PackageImpl manifest(String name) {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.setProcessName(name);
        return parsed;
    }
    private static ApkPackage apk(PackageImpl parsed, int uid) throws Exception {
        parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage)unsafe().allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(pkg, parsed.getPackageName());
        field(ApkPackage.class, "parsed").set(pkg, parsed);
        field(ApkPackage.class, "apkPath").set(pkg, parsed.getBaseApkPath());
        field(ApkPackage.class, "mDataDir").set(pkg, "/fixture/" + parsed.getPackageName() + "/data");
        field(ApkPackage.class, "mUid").setInt(pkg, uid);
        field(ApkPackage.class, "sharedLibraries").set(pkg, SharedLibraryGraph.EMPTY);
        field(ApkPackage.class, "mSigningIdentityChecked").setBoolean(pkg, true);
        field(ApkPackage.class, "mVerifiedSignatures").set(pkg,
                new Signature[] { new Signature(new byte[] { 1 }) });
        return pkg;
    }
    private static ApkPackage apk(String name, String authority, int uid, boolean counted) throws Exception {
        PackageImpl parsed = counted ? new CountingPackage(name) : manifest(name);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.setProcessName(name);
        provider(parsed, "Provider", authority, true, false);
        return apk(parsed, uid);
    }
    private static LinkedHashMap<String, ApkPackage> map(ApkPackage... values) {
        LinkedHashMap<String, ApkPackage> result = new LinkedHashMap<>();
        for (ApkPackage value : values) result.put(value.packageName, value);
        return result;
    }
    private static PackageManagerService pm(Map<String, ApkPackage> packages) throws Exception {
        PackageManagerService result = (PackageManagerService)unsafe().allocateInstance(PackageManagerService.class);
        field(PackageManagerService.class, "mPackages").set(result, packages);
        field(PackageManagerService.class, "mRemoving").set(result, Map.of());
        // Publication may retire other package-generation indices as well. Keep
        // the real policy owner attached even though this fixture does no grants.
        var policy = Class.forName("com.gesher.server.PermissionPolicy")
                .getDeclaredConstructor(PackageManagerService.class);
        policy.setAccessible(true);
        field(PackageManagerService.class, "mPermissions").set(result, policy.newInstance(result));
        return result;
    }
    private static void publish(PackageManagerService pm, Map<String, ApkPackage> packages) throws Exception {
        synchronized (pm) {
            invoke(method(PackageManagerService.class, "publishPackages", Map.class), pm, packages);
        }
    }
    private static ProviderInfo declared(PackageManagerService pm, String authority) throws Exception {
        return (ProviderInfo)invoke(method(Class.forName("com.gesher.server.ContentProviders"),
                "declaredProvider", PackageManagerService.class, String.class), null, pm, authority);
    }
    private static void conflict(PackageManagerService pm, String authority) throws Exception {
        try { declared(pm, authority); throw new AssertionError("Ambiguous authority selected an owner"); }
        catch (SecurityException expected) { checks++; }
    }

    public static void contracts() throws Exception {
        ApkPackage first = apk("fixture.authority.first", "shared;alias", 12341, false);
        ApkPackage second = apk("fixture.authority.second", "shared", 12342, false);
        PackageManagerService pm = pm(map(first));
        ProviderInfo actual = declared(pm, "shared");
        require(actual != null && first.packageName.equals(actual.packageName)
                        && actual.applicationInfo.uid == 12341
                        && (first.packageName + ".READ").equals(actual.readPermission)
                        && first.packageName.equals(actual.metaData.getString("owner")), "Canonical metadata changed");
        require(declared(pm, "alias").name.equals(actual.name), "Secondary authority lost");
        require(pm.resolveContentProvider("shared", 0, 1) == null, "Other user's provider leaked");
        require(declared(pm, null) == null && declared(pm, "absent") == null, "Missing provider fabricated");
        require(pm.resolveContentProvider("shared", 0, 0).metaData == null, "GET_META_DATA ignored");
        ProviderInfo copy = pm.resolveContentProvider("shared", PackageManager.GET_META_DATA, 0);
        copy.name = "mutated"; copy.authority = "mutated"; copy.applicationInfo.uid = -1;
        ProviderInfo fresh = pm.resolveContentProvider("shared", PackageManager.GET_META_DATA, 0);
        require(!"mutated".equals(fresh.name) && "shared;alias".equals(fresh.authority)
                        && fresh.applicationInfo.uid == 12341 && fresh != copy
                        && fresh.applicationInfo != copy.applicationInfo, "Shared mutable query result");

        Method component = method(ApkPackage.class, "setComponentEnabledState", String.class, int.class);
        invoke(component, first, first.packageName + ".Provider", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
        require(declared(pm, "shared") == null, "Index cached enabled state");
        require(pm.resolveContentProvider("shared", PackageManager.MATCH_DISABLED_COMPONENTS, 0) != null,
                "Disabled-match flags were cached");
        invoke(component, first, first.packageName + ".Provider", PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
        require(declared(pm, "shared") != null, "Re-enabled provider stayed absent");
        Object enabled = invoke(method(ApkPackage.class, "enabledState"), first);
        Method application = method(enabled.getClass(), "application", int.class);
        field(ApkPackage.class, "mLocalEnabledState").set(first,
                invoke(application, enabled, PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER));
        require(declared(pm, "shared") == null
                        && pm.resolveContentProvider("shared", PackageManager.MATCH_DISABLED_COMPONENTS, 0) != null,
                "Application enable policy was cached");
        field(ApkPackage.class, "mLocalEnabledState").set(first,
                invoke(application, enabled, PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED));
        require(declared(pm, "shared") == null && pm.resolveContentProvider("shared",
                        PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS, 0) != null,
                "Disabled-until-used query flags were lost");
        field(ApkPackage.class, "mLocalEnabledState").set(first, enabled);
        require(declared(pm, "shared") != null, "Restored application stayed absent");
        publish(pm, map(first, second)); conflict(pm, "shared");
        require(pm.resolveContentProvider("shared", 0, 0).packageName.equals(first.packageName),
                "PackageManager first-owner order changed");
        synchronized (pm) { field(PackageManagerService.class, "mRemoving").set(pm, map(first)); }
        require(declared(pm, "shared").packageName.equals(second.packageName), "Removing owner was used");
        synchronized (pm) { field(PackageManagerService.class, "mRemoving").set(pm, Map.of()); }
        conflict(pm, "shared");
        publish(pm, map(second, first));
        require(pm.resolveContentProvider("shared", 0, 0).packageName.equals(second.packageName),
                "Installed order changed");

        ApkPackage replacement = apk(first.packageName, "replacement", 12349, false);
        publish(pm, map(replacement));
        require(field(PackageManagerService.class, "mProviderAuthorityIndex").get(pm) == null,
                "Published replacement retained old code in its index");
        require(declared(pm, "shared") == null && declared(pm, "replacement").applicationInfo.uid == 12349,
                "Replacement kept stale authority/UID");
        publish(pm, Map.of());
        require(declared(pm, "replacement") == null, "Uninstall retained an owner");
        publish(pm, map(first));
        require(declared(pm, "shared").applicationInfo.uid == 12341, "Reinstall retained previous generation");

        PackageImpl parsed = manifest("fixture.authority.duplicates");
        provider(parsed, "Canonical", "canonical", false, false);
        provider(parsed, "Canonical", "ignored", true, true);
        provider(parsed, "Aware", "aware", true, true);
        provider(parsed, "One", "same", true, false);
        provider(parsed, "Two", "same", true, false);
        ApkPackage duplicates = apk(parsed, 12350);
        publish(pm, map(duplicates));
        require(declared(pm, "canonical") == null && declared(pm, "ignored") == null,
                "Duplicate component overrode first disabled declaration");
        require(pm.resolveContentProvider("canonical", PackageManager.MATCH_DISABLED_COMPONENTS, 0) != null,
                "Canonical disabled declaration disappeared");
        require(pm.resolveContentProvider("canonical", PackageManager.MATCH_DISABLED_COMPONENTS
                | PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) == null, "Later boot-aware duplicate replaced first");
        require(pm.resolveContentProvider("aware", PackageManager.MATCH_DIRECT_BOOT_UNAWARE, 0) == null
                && pm.resolveContentProvider("aware", PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) != null,
                "Direct-boot flags were cached");
        sameAuthority(pm, duplicates);

        // Thousands of distinct misses must not grow a result/negative-result cache.
        Object index = field(PackageManagerService.class, "mProviderAuthorityIndex").get(pm);
        Map<?, ?> owners = (Map<?, ?>)field(index.getClass(), "owners").get(index);
        int size = owners.size();
        for (int i = 0; i < 2000; i++) {
            if (declared(pm, "missing." + i) != null) throw new AssertionError("Unknown authority resolved");
        }
        require(owners.size() == size, "Negative queries grew the index");
        try {
            ((List<?>)invoke(method(PackageManagerService.class, "providerPackages", String.class), pm, "same")).clear();
            throw new AssertionError("Candidate list is mutable");
        } catch (UnsupportedOperationException expected) { checks++; }
        concurrency(first, replacement);
        System.out.println("ProviderAuthorityIndexTest all passed: " + checks + " checks");
    }

    private static ProviderInfo component(PackageManagerService pm, ApkPackage pkg, String name, long flags) {
        return pm.getProviderInfo(new android.content.ComponentName(pkg.packageName,
                pkg.packageName + "." + name), flags, 0);
    }

    /**
     * ComponentResolver.addProvidersLocked: within one package the first declaration of an
     * authority keeps it and a later one is skipped, by manifest order and regardless of
     * enabled state; INSTALL_FAILED_CONFLICTING_PROVIDER still separates packages.
     */
    private static void sameAuthority(PackageManagerService pm, ApkPackage duplicates) throws Exception {
        ProviderInfo same = declared(pm, "same");
        require(same != null && same.name.equals(duplicates.packageName + ".One")
                        && "same".equals(same.authority), "Same-package duplicate authority did not keep its first provider");
        require(pm.resolveContentProvider("same", 0, 0).name.equals(same.name),
                "PackageManager authority lookup disagrees with registration");
        ProviderInfo skipped = component(pm, duplicates, "Two", 0);
        require(skipped != null && skipped.authority == null,
                "Skipped duplicate kept a registered authority");
        Method componentState = method(ApkPackage.class, "setComponentEnabledState", String.class, int.class);
        invoke(componentState, duplicates, duplicates.packageName + ".One", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
        require(declared(pm, "same") == null, "Disabled first provider handed its authority to a later declaration");
        invoke(componentState, duplicates, duplicates.packageName + ".One", PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);

        PackageImpl parsed = manifest("fixture.authority.multi");
        provider(parsed, "First", "x;y", true, false);
        provider(parsed, "Second", "y;z;z", true, false);
        provider(parsed, "Third", "x", true, false);
        provider(parsed, "Fourth", "w;x", true, false);
        ApkPackage multi = apk(parsed, 12351);
        publish(pm, map(multi));
        require(declared(pm, "x").name.equals(multi.packageName + ".First")
                        && declared(pm, "y").name.equals(multi.packageName + ".First"),
                "First multi-authority declaration lost an authority");
        ProviderInfo second = declared(pm, "z");
        require(second != null && second.name.equals(multi.packageName + ".Second") && "z".equals(second.authority),
                "Partially skipped provider did not keep exactly its free authorities");
        require(component(pm, multi, "Third", 0).authority == null, "Fully skipped provider kept an authority");
        require("w".equals(declared(pm, "w").authority) && declared(pm, "w").name.equals(multi.packageName + ".Fourth"),
                "Free authority of a partially skipped provider was not registered");
        require("x;y".equals(component(pm, multi, "First", 0).authority), "First provider authority list changed");

        ApkPackage other = apk("fixture.authority.other", "z", 12352, false);
        publish(pm, map(multi, other));
        conflict(pm, "z");
        require(declared(pm, "x").packageName.equals(multi.packageName), "Unrelated authority became ambiguous");

        ApkPackage otherDisabledOwner = apk("fixture.authority.otherdisabled", "x", 12357, false);
        invoke(componentState, multi, multi.packageName + ".First", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
        publish(pm, map(multi, otherDisabledOwner));
        conflict(pm, "x");

        // Registration happens over both boot states, whatever boot state a query asks for.
        PackageImpl boot = manifest("fixture.authority.boot");
        provider(boot, "Unaware", "d", true, false);
        provider(boot, "Aware", "d;e", true, true);
        ApkPackage bootPkg = apk(boot, 12354);
        publish(pm, map(bootPkg));
        android.content.pm.PackageInfo aware = pm.getPackageInfo(bootPkg.packageName,
                PackageManager.GET_PROVIDERS | PackageManager.MATCH_DIRECT_BOOT_AWARE, 0);
        require(aware.providers != null && aware.providers.length == 1
                        && aware.providers[0].name.equals(bootPkg.packageName + ".Aware")
                        && "e".equals(aware.providers[0].authority),
                "Direct-boot-aware query re-registered an authority an unaware declaration owns");
        android.content.pm.PackageInfo unaware = pm.getPackageInfo(bootPkg.packageName,
                PackageManager.GET_PROVIDERS | PackageManager.MATCH_DIRECT_BOOT_UNAWARE, 0);
        require(unaware.providers != null && unaware.providers.length == 1 && "d".equals(unaware.providers[0].authority),
                "Direct-boot-unaware query lost the registered declaration");
        android.content.pm.PackageInfo both = pm.getPackageInfo(bootPkg.packageName, PackageManager.GET_PROVIDERS, 0);
        require(both.providers != null && both.providers.length == 2 && "d".equals(both.providers[0].authority)
                        && "e".equals(both.providers[1].authority), "Default boot-state query disagrees with registration");
        require(pm.resolveContentProvider("d", PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) == null
                        && pm.resolveContentProvider("e", PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) != null,
                "Direct-boot authority lookup disagrees with registration");
        PackageImpl repeated = manifest("fixture.authority.repeated");
        provider(repeated, "First", "original", true, false);
        provider(repeated, "First", "other", true, false);
        provider(repeated, "Second", "other", true, false);
        ApkPackage repeatedPkg = apk(repeated, 12355);
        publish(pm, map(repeatedPkg));
        ProviderInfo otherOwner = declared(pm, "other");
        require(otherOwner != null && otherOwner.name.equals(repeatedPkg.packageName + ".Second"),
                "An ignored declaration of the same class claimed another provider's authority");

        PackageImpl syncable = manifest("fixture.authority.syncable");
        provider(syncable, "First", "shared", true, false);
        provider(syncable, "Second", "shared;alias", true, false).setSyncable(true);
        provider(syncable, "Third", "own;more;extra", true, false).setSyncable(true);
        provider(syncable, "Fourth", "fourth;more", true, false).setSyncable(true);
        provider(syncable, "Fifth", "shared;own", true, false).setSyncable(true);
        ApkPackage syncablePkg = apk(syncable, 12356);
        publish(pm, map(syncablePkg));
        // ComponentResolver: a skipped first authority leaves the free ones to the
        // copy registered for the later authorities, whose isSyncable is false.
        ProviderInfo shared = declared(pm, "shared");
        require(shared != null && shared.name.equals(syncablePkg.packageName + ".First"),
                "Syncable multi-authority provider took an authority registered earlier");
        ProviderInfo alias = declared(pm, "alias");
        require(alias != null && alias.name.equals(syncablePkg.packageName + ".Second")
                        && "alias".equals(alias.authority) && !alias.isSyncable,
                "Free authority of a syncable provider with a skipped first authority");
        ProviderInfo own = declared(pm, "own");
        require(own != null && own.name.equals(syncablePkg.packageName + ".Third")
                        && "own;more;extra".equals(own.authority) && own.isSyncable,
                "Syncable provider that registered its first authority changed");
        ProviderInfo firstRecord = pm.resolveContentProvider("own", 0, 0);
        require(firstRecord != null && "own".equals(firstRecord.authority) && firstRecord.isSyncable,
                "Authority lookup did not retain the syncable class record");
        ProviderInfo aliasRecord = pm.resolveContentProvider("more", 0, 0);
        require(aliasRecord != null && "more;extra".equals(aliasRecord.authority) && !aliasRecord.isSyncable,
                "Secondary authority lookup exposed the syncable class record");
        ProviderInfo skippedClass = component(pm, syncablePkg, "Second", 0);
        require(skippedClass != null && skippedClass.authority == null && skippedClass.isSyncable,
                "Class lookup exposed the non-syncable copy of a skipped first authority");
        ProviderInfo freeAlias = pm.resolveContentProvider("alias", 0, 0);
        require(freeAlias != null && "alias".equals(freeAlias.authority) && !freeAlias.isSyncable,
                "Authority lookup lost the free alias after skipping the first authority");
        ProviderInfo fourth = declared(pm, "fourth");
        require(fourth != null && "fourth".equals(fourth.authority) && fourth.isSyncable,
                "Syncable provider keeping its first authority lost its syncable state");
        ProviderInfo fifth = component(pm, syncablePkg, "Fifth", 0);
        require(fifth != null && fifth.authority == null && fifth.isSyncable,
                "Fully skipped syncable provider kept an authority");

        publish(pm, map(duplicates));
        require(declared(pm, "same").name.equals(same.name), "Republished package changed its registered provider");
    }

    private static void concurrency(ApkPackage first, ApkPackage replacement) throws Exception {
        PackageManagerService pm = pm(map(first));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch start = new CountDownLatch(1);
        List<Thread> readers = new ArrayList<>();
        for (int r = 0; r < 4; r++) {
            Thread reader = new Thread(() -> {
                try {
                    start.await();
                    for (int i = 0; i < 300; i++) {
                        ProviderInfo result = declared(pm, (i & 1) == 0 ? "shared" : "replacement");
                        if (result != null && !("shared;alias".equals(result.authority) && result.applicationInfo.uid == 12341)
                                && !("replacement".equals(result.authority) && result.applicationInfo.uid == 12349)) {
                            throw new AssertionError("Mixed installed generations");
                        }
                    }
                } catch (Throwable problem) { failure.compareAndSet(null, problem); }
            }, "provider-authority-reader");
            reader.setDaemon(true); reader.start(); readers.add(reader);
        }
        start.countDown();
        for (int i = 0; i < 300; i++) publish(pm, map((i & 1) == 0 ? replacement : first));
        for (Thread reader : readers) {
            reader.join(10_000); require(!reader.isAlive(), "Provider generation reader deadlocked");
        }
        if (failure.get() != null) throw new AssertionError("Concurrent provider publication failed", failure.get());
    }

    /** Counts real parsed-provider reads performed by unchanged Android metadata generation. */
    public static void benchmark(boolean requireIndex) throws Exception {
        int singleOwnerReads = 0;
        for (int count : new int[] { 1, 50, 500, 1000 }) {
            LinkedHashMap<String, ApkPackage> packages = new LinkedHashMap<>();
            for (int i = 0; i < count; i++) {
                ApkPackage pkg = apk("fixture.authority.bench" + i, "authority." + i, 20000 + i, true);
                packages.put(pkg.packageName, pkg);
            }
            PackageManagerService pm = pm(packages);
            String target = "authority." + (count - 1);
            providerReads.set(0);
            require(declared(pm, target) != null, "Cold declaration lost");
            int coldReads = providerReads.getAndSet(0);
            for (int i = 0; i < 4; i++) { declared(pm, target); pm.resolveContentProvider(target, 0, 0); }
            providerReads.set(0);
            int iterations = 32;
            long started = System.nanoTime();
            for (int i = 0; i < iterations; i++) {
                ProviderInfo result = declared(pm, target);
                require(result != null && result.applicationInfo.uid == 20000 + count - 1, "Benchmark result differs");
            }
            long declarationNs = System.nanoTime() - started;
            int declarations = providerReads.getAndSet(0);
            for (int i = 0; i < iterations; i++) require(pm.resolveContentProvider(target, 0, 0) != null, "PMS result lost");
            int resolutions = providerReads.getAndSet(0);
            for (int i = 0; i < iterations; i++) require(declared(pm, "not-installed") == null, "Miss resolved");
            int misses = providerReads.getAndSet(0);
            System.out.println("PROVIDER_WORK packages=" + count + " queries=" + iterations
                    + " cold_reads=" + coldReads + " declarations=" + declarations + " resolutions=" + resolutions
                    + " misses=" + misses + " declaration_ns=" + declarationNs);
            if (count == 1) singleOwnerReads = declarations;
            if (requireIndex) {
                require(singleOwnerReads > 0 && declarations == singleOwnerReads
                                && resolutions == singleOwnerReads && misses == 0,
                        "Provider lookup visited unrelated package metadata");
            }
        }
    }

    public static void main(String[] args) throws Exception { contracts(); benchmark(true); }
}
