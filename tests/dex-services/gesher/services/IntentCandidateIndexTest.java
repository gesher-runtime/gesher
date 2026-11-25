/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Bundle;
import android.os.PatternMatcher;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.*;
import com.gesher.server.ApkPackage;
import com.gesher.server.BrowserPackageSupport;
import com.gesher.server.GmsPackageSupport;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Actual PMS, ApkPackage and Android IntentFilter/metadata; constructed manifests. */
public final class IntentCandidateIndexTest {
    private enum Kind { ACTIVITY, SERVICE, RECEIVER, PROVIDER }
    private static final String ACTION = "fixture.intent.SHARED", RICH = "fixture.intent.RICH";
    private static final AtomicInteger visits = new AtomicInteger(), reads = new AtomicInteger();
    private static int checks;

    private static void check(boolean value, String reason) {
        checks++; if (!value) throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method result = type.getDeclaredMethod(name, parameters); result.setAccessible(true); return result;
    }
    private static Object invoke(Method method, Object target, Object... args) throws Exception {
        try { return method.invoke(target, args); }
        catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            if (cause instanceof Error) throw (Error)cause;
            throw failure;
        }
    }
    private static Unsafe unsafe() throws Exception { return (Unsafe)field(Unsafe.class, "theUnsafe").get(null); }

    /** Counters observe the production iteration/read boundaries without changing values. */
    private static final class CountedPackages extends LinkedHashMap<String, ApkPackage> {
        @Override public Collection<ApkPackage> values() {
            Collection<ApkPackage> values = super.values();
            return new AbstractCollection<>() {
                public int size() { return values.size(); }
                public Iterator<ApkPackage> iterator() {
                    Iterator<ApkPackage> iterator = values.iterator();
                    return new Iterator<>() {
                        public boolean hasNext() { return iterator.hasNext(); }
                        public ApkPackage next() { visits.incrementAndGet(); return iterator.next(); }
                    };
                }
            };
        }
    }
    private static final class CountedPackage extends PackageImpl {
        CountedPackage(String name) {
            super(name, "/fixture/" + name + "/base.apk", "/fixture/" + name + "/base.apk", null, false, null);
        }
        @Override public List<ParsedActivity> getActivities() { reads.incrementAndGet(); return super.getActivities(); }
        @Override public List<ParsedActivity> getReceivers() { reads.incrementAndGet(); return super.getReceivers(); }
        @Override public List<ParsedService> getServices() { reads.incrementAndGet(); return super.getServices(); }
        @Override public List<ParsedProvider> getProviders() { reads.incrementAndGet(); return super.getProviders(); }
    }
    private static PackageImpl manifest(String name, boolean counted) {
        PackageImpl parsed = counted ? new CountedPackage(name)
                : (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.setProcessName(name); return parsed;
    }
    private static ParsedIntentInfoImpl filter(String action, boolean defaultCategory) {
        ParsedIntentInfoImpl info = new ParsedIntentInfoImpl();
        if (action != null) info.getIntentFilter().addAction(action);
        if (defaultCategory) info.getIntentFilter().addCategory(Intent.CATEGORY_DEFAULT);
        info.setHasDefault(defaultCategory); return info;
    }
    private static void declaration(PackageImpl parsed, Kind kind, String suffix,
            boolean enabled, boolean aware, ParsedIntentInfoImpl... filters) {
        ParsedMainComponentImpl component;
        if (kind == Kind.SERVICE) component = new ParsedServiceImpl();
        else if (kind == Kind.PROVIDER) component = new ParsedProviderImpl();
        else component = new ParsedActivityImpl();
        component.setName(parsed.getPackageName() + "." + suffix);
        component.setPackageName(parsed.getPackageName()); component.setProcessName(parsed.getPackageName());
        component.setEnabled(enabled); component.setExported(true); component.setDirectBootAware(aware);
        Bundle metadata = new Bundle(); metadata.putString("declaration", suffix); component.setMetaData(metadata);
        for (ParsedIntentInfoImpl info : filters) component.addIntent(info);
        switch (kind) {
            case ACTIVITY -> parsed.addActivity((ParsedActivityImpl)component);
            case RECEIVER -> parsed.addReceiver((ParsedActivityImpl)component);
            case SERVICE -> parsed.addService((ParsedServiceImpl)component);
            case PROVIDER -> {
                ((ParsedProviderImpl)component).setAuthority(component.getName() + ".items");
                parsed.addProvider((ParsedProviderImpl)component);
            }
        }
    }
    private static ApkPackage apk(PackageImpl parsed, int uid) throws Exception {
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)unsafe().allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(result, parsed.getPackageName());
        field(ApkPackage.class, "parsed").set(result, parsed);
        field(ApkPackage.class, "apkPath").set(result, parsed.getBaseApkPath());
        field(ApkPackage.class, "mDataDir").set(result, "/fixture/" + parsed.getPackageName() + "/data");
        field(ApkPackage.class, "mUid").setInt(result, uid);
        field(ApkPackage.class, "sharedLibraries").set(result, SharedLibraryGraph.EMPTY);
        field(ApkPackage.class, "mSigningIdentityChecked").setBoolean(result, true);
        field(ApkPackage.class, "mVerifiedSignatures").set(result, new Signature[]{new Signature(new byte[]{1})});
        return result;
    }
    private static ApkPackage single(String name, String action, int uid, boolean counted) throws Exception {
        PackageImpl parsed = manifest(name, counted);
        for (Kind kind : Kind.values()) declaration(parsed, kind, kind.name(), true, false, filter(action, true));
        return apk(parsed, uid);
    }
    private static CountedPackages packages(ApkPackage... values) {
        CountedPackages result = new CountedPackages();
        for (ApkPackage pkg : values) result.put(pkg.packageName, pkg); return result;
    }
    private static PackageManagerService pm(Map<String, ApkPackage> packages) throws Exception {
        PackageManagerService result = (PackageManagerService)unsafe().allocateInstance(PackageManagerService.class);
        field(PackageManagerService.class, "mPackages").set(result, packages);
        field(PackageManagerService.class, "mRemoving").set(result, Map.of());
        var policy = Class.forName("com.gesher.server.PermissionPolicy").getDeclaredConstructor(PackageManagerService.class);
        policy.setAccessible(true);
        field(PackageManagerService.class, "mPermissions").set(result, policy.newInstance(result));
        return result;
    }
    private static void publish(PackageManagerService pm, Map<String, ApkPackage> packages) throws Exception {
        synchronized (pm) { invoke(method(PackageManagerService.class, "publishPackages", Map.class), pm, packages); }
    }
    @SuppressWarnings("unchecked")
    private static List<ResolveInfo> query(PackageManagerService pm, Kind kind, Intent intent, String type, long flags) {
        return switch (kind) {
            case ACTIVITY -> pm.queryIntentActivities(intent, type, flags, 0).getList();
            case SERVICE -> pm.queryIntentServices(intent, type, flags, 0).getList();
            case RECEIVER -> pm.queryIntentReceivers(intent, type, flags, 0).getList();
            case PROVIDER -> pm.queryIntentContentProviders(intent, type, flags, 0).getList();
        };
    }
    private static List<ResolveInfo> declared(ApkPackage pkg, Kind kind, Intent intent, String type, long flags) {
        return switch (kind) {
            case ACTIVITY -> pkg.queryIntentActivities(intent, type, flags);
            case SERVICE -> pkg.queryIntentServices(intent, type, flags);
            case RECEIVER -> pkg.queryIntentReceivers(intent, type, flags);
            case PROVIDER -> pkg.queryIntentContentProviders(intent, type, flags);
        };
    }
    /** The pre-index scan, retaining the production family's matching and fallback rules. */
    private static List<ResolveInfo> scan(Map<String, ApkPackage> packages, Set<String> removing,
            Kind kind, Intent intent, String type, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage pkg : packages.values()) {
            if (!removing.contains(pkg.packageName)) result.addAll(declared(pkg, kind, intent, type, flags));
        }
        if (kind == Kind.ACTIVITY && !packages.containsKey(BrowserPackageSupport.PACKAGE)) {
            result.addAll(BrowserPackageSupport.queryIntentActivities(intent, type, flags));
        }
        if (kind == Kind.SERVICE && result.isEmpty() && !packages.containsKey(GmsPackageSupport.PACKAGE_GMS)) {
            result.addAll(GmsPackageSupport.querySyntheticServices(intent, type, flags));
        }
        return result;
    }
    private static ComponentInfo info(ResolveInfo result) {
        return result.activityInfo != null ? result.activityInfo
                : result.serviceInfo != null ? result.serviceInfo : result.providerInfo;
    }
    private static List<String> describe(List<ResolveInfo> results) {
        List<String> values = new ArrayList<>();
        for (ResolveInfo result : results) {
            ComponentInfo component = info(result);
            List<String> actions = new ArrayList<>();
            if (result.filter != null) for (int i=0; i<result.filter.countActions(); i++) actions.add(result.filter.getAction(i));
            values.add(component.packageName + "/" + component.name + ":" + component.applicationInfo.uid
                    + ":" + component.enabled + ":" + component.applicationInfo.enabled + ":" + component.directBootAware
                    + ":" + component.exported + ":" + component.processName + ":" + component.metaData
                    + ":" + result.match + ":" + result.priority + ":" + result.isDefault
                    + ":" + (result.filter != null) + ":" + actions);
        }
        return values;
    }
    private static void parity(PackageManagerService pm, Map<String, ApkPackage> packages, Set<String> removing,
            Kind kind, Intent intent, String type, long flags) {
        check(describe(query(pm, kind, intent, type, flags)).equals(describe(scan(packages, removing, kind, intent, type, flags))),
                "Indexed result differs from canonical scan: " + kind + " " + intent + " flags=" + flags + " type=" + type);
    }

    public static void contracts(boolean indexed) throws Exception {
        PackageImpl rich = manifest("fixture.intent.first", false);
        for (Kind kind : Kind.values()) {
            declaration(rich, kind, kind + "Main", true, false, filter(ACTION, true), filter(ACTION, false));
            declaration(rich, kind, kind + "Plain", true, false);
            declaration(rich, kind, kind + "Disabled", false, false, filter(ACTION, true));
            declaration(rich, kind, kind + "Aware", true, true, filter(ACTION, false));
            declaration(rich, kind, kind + "Zero", true, false, filter(null, true));
            ParsedIntentInfoImpl typed = filter(RICH, true);
            typed.getIntentFilter().addCategory(Intent.CATEGORY_BROWSABLE);
            typed.getIntentFilter().addDataType("image/*"); typed.getIntentFilter().addDataScheme("content");
            typed.getIntentFilter().addDataAuthority("fixture", "123");
            typed.getIntentFilter().addDataPath("/allowed", PatternMatcher.PATTERN_PREFIX);
            declaration(rich, kind, kind + "Typed", true, false, typed);
        }
        ApkPackage first = apk(rich, 12341), second = single("fixture.intent.second", ACTION, 12342, false);
        PackageImpl zero = manifest("fixture.intent.zero", false), plain = manifest("fixture.intent.plain", false);
        PackageImpl separated = manifest("fixture.intent.families", false);
        for (Kind kind : Kind.values()) {
            declaration(zero, kind, kind.name(), true, false, filter(null, false));
            declaration(plain, kind, kind.name(), true, false);
            declaration(separated, kind, kind.name(), true, false, filter("fixture.kind." + kind, true));
        }
        ApkPackage zeroOwner = apk(zero, 12343), plainOwner = apk(plain, 12344), familyOwner = apk(separated, 12345);
        CountedPackages packages = packages(first, second, zeroOwner, plainOwner, familyOwner);
        PackageManagerService pm = pm(packages);
        for (Kind kind : Kind.values()) {
            Intent explicit = new Intent("no.filter").setComponent(new ComponentName(plainOwner.packageName, plainOwner.packageName + "." + kind));
            check(query(pm, kind, explicit, null, 0).size() == 1, "Explicit component required an intent filter");
        }
        if (indexed) check(field(PackageManagerService.class, "mIntentPackageIndex").get(pm) == null,
                "Explicit query built an unrelated declaration index");
        Intent[] intents = {
                new Intent(ACTION), new Intent(), new Intent("fixture.intent.ABSENT"),
                new Intent(ACTION).setPackage(first.packageName), new Intent(ACTION).setPackage("absent"),
                new Intent(ACTION).addCategory("missing.category"),
                new Intent(RICH).setDataAndType(Uri.parse("content://fixture:123/allowed/image"), "image/png")
                        .addCategory(Intent.CATEGORY_BROWSABLE),
                new Intent(RICH).setDataAndType(Uri.parse("content://fixture:123/denied/image"), "image/png"),
                new Intent(RICH).setDataAndType(Uri.parse("content://fixture:123/allowed/image"), "text/plain"),
                new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.invalid/path")),
                new Intent("fixture.kind.ACTIVITY"), new Intent("fixture.kind.SERVICE"),
                new Intent("fixture.kind.RECEIVER"), new Intent("fixture.kind.PROVIDER")
        };
        long[] flags = {0, PackageManager.GET_META_DATA | PackageManager.GET_RESOLVED_FILTER,
                PackageManager.MATCH_DEFAULT_ONLY, PackageManager.MATCH_DIRECT_BOOT_AWARE,
                PackageManager.MATCH_DIRECT_BOOT_UNAWARE, PackageManager.MATCH_DISABLED_COMPONENTS};
        for (Kind kind : Kind.values()) {
            for (Intent intent : intents) for (long flag : flags) {
                parity(pm, packages, Set.of(), kind, intent, null, flag);
                parity(pm, packages, Set.of(), kind, intent, "image/png", flag);
            }
            check(query(pm, kind, new Intent(), null, 0).stream()
                    .anyMatch(result -> info(result).packageName.equals(zeroOwner.packageName)),
                    "Null-action query lost a package containing only zero-action filters");
            check(query(pm, kind, new Intent("fixture.kind." + kind), null, 0).stream()
                    .anyMatch(result -> info(result).packageName.equals(familyOwner.packageName)),
                    "Action was indexed under the wrong component family");
            Intent mismatch = new Intent(ACTION).setPackage(second.packageName).setComponent(
                    new ComponentName(first.packageName, first.packageName + "." + kind + "Plain"));
            check(query(pm, kind, mismatch, null, 0).isEmpty(), "Conflicting package/component was admitted");
            List<ResolveInfo> results = query(pm, kind, new Intent(ACTION), null, PackageManager.GET_RESOLVED_FILTER);
            ComponentInfo old = info(results.get(0)); old.name = "changed"; old.applicationInfo.uid = -1;
            results.get(0).filter.addAction("injected");
            ResolveInfo fresh = query(pm, kind, new Intent(ACTION), null, PackageManager.GET_RESOLVED_FILTER).get(0);
            check(!info(fresh).name.equals("changed") && info(fresh).applicationInfo.uid == 12341
                    && !fresh.filter.hasAction("injected"), "Mutable query results were retained");
            String name = first.packageName + "." + kind + "Main";
            invoke(method(ApkPackage.class, "setComponentEnabledState", String.class, int.class), first, name,
                    PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
            parity(pm, packages, Set.of(), kind, new Intent(ACTION), null, 0);
            parity(pm, packages, Set.of(), kind, new Intent(ACTION), null, PackageManager.MATCH_DISABLED_COMPONENTS);
            invoke(method(ApkPackage.class, "setComponentEnabledState", String.class, int.class), first, name,
                    PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
        }
        Object index = indexed ? field(PackageManagerService.class, "mIntentPackageIndex").get(pm) : null;
        Object state = invoke(method(ApkPackage.class, "enabledState"), first);
        Method application = method(state.getClass(), "application", int.class);
        for (int setting : new int[]{PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER,
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED}) {
            field(ApkPackage.class, "mLocalEnabledState").set(first, invoke(application, state, setting));
            for (Kind kind : Kind.values()) for (long flag : new long[]{0, PackageManager.MATCH_DISABLED_COMPONENTS,
                    PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS}) parity(pm, packages, Set.of(), kind, new Intent(ACTION), null, flag);
        }
        field(ApkPackage.class, "mLocalEnabledState").set(first, state);
        if (indexed) {
            check(field(PackageManagerService.class, "mIntentPackageIndex").get(pm) == index, "Live state grew a separate index generation");
            Map<?, ?> families = (Map<?, ?>)field(index.getClass(), "families").get(index);
            check(families.size() == 4, "Index family cardinality");
            for (int i = 0; i < 2000; i++) query(pm, Kind.PROVIDER, new Intent("missing." + i), null, i);
            for (Object candidates : families.values()) {
                Map<?, ?> actions = (Map<?, ?>)field(candidates.getClass(), "actions").get(candidates);
                check(actions.size() == 3, "Unknown actions accumulated in index");
                try { ((List<?>)actions.get(ACTION)).clear(); throw new AssertionError("Mutable owner candidates"); }
                catch (UnsupportedOperationException expected) { checks++; }
            }
        }
        synchronized (pm) { field(PackageManagerService.class, "mRemoving").set(pm, Map.of(first.packageName, first)); }
        for (Kind kind : Kind.values()) parity(pm, packages, Set.of(first.packageName), kind, new Intent(ACTION), null, 0);
        synchronized (pm) { field(PackageManagerService.class, "mRemoving").set(pm, Map.of()); }
        publish(pm, packages(second, first));
        for (Kind kind : Kind.values()) check(info(query(pm, kind, new Intent(ACTION), null, 0).get(0)).packageName.equals(second.packageName),
                "Installed package order changed");
        ApkPackage replacement = single(first.packageName, "replacement", 12349, false);
        publish(pm, packages(replacement));
        if (indexed) check(field(PackageManagerService.class, "mIntentPackageIndex").get(pm) == null, "Publication retained retired index");
        for (Kind kind : Kind.values()) {
            parity(pm, packages(replacement), Set.of(), kind, new Intent(ACTION), null, 0);
            check(info(query(pm, kind, new Intent("replacement"), null, 0).get(0)).applicationInfo.uid == 12349, "Replacement UID/action stayed stale");
        }
        publish(pm, Map.of());
        for (Kind kind : Kind.values()) parity(pm, Map.of(), Set.of(), kind, new Intent("replacement"), null, 0);
        publish(pm, packages(first));
        for (Kind kind : Kind.values()) check(info(query(pm, kind, new Intent(ACTION), null, 0).get(0)).applicationInfo.uid == 12341, "Reinstall stayed stale");
        concurrent();
        System.out.println("IntentCandidateIndexTest contracts passed: " + checks + " checks");
    }
    private static void concurrent() throws Exception {
        ApkPackage first = single("fixture.intent.concurrent", ACTION, 14001, false);
        ApkPackage replacement = single(first.packageName, ACTION, 14002, false);
        PackageManagerService pm = pm(packages(first));
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        List<Thread> readers = new ArrayList<>();
        for (int n = 0; n < 3; n++) {
            Thread reader = new Thread(() -> {
                try {
                    start.await();
                    for (int i = 0; i < 200; i++) for (Kind kind : Kind.values()) {
                        List<ResolveInfo> found = query(pm, kind, new Intent(ACTION), null, 0);
                        if (found.size() != 1) throw new AssertionError("Concurrent generation lost an owner");
                        int uid = info(found.get(0)).applicationInfo.uid;
                        if (uid != 14001 && uid != 14002) throw new AssertionError("Concurrent result used an invalid UID");
                    }
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            }); reader.setDaemon(true); reader.start(); readers.add(reader);
        }
        start.countDown();
        for (int i = 0; i < 200; i++) publish(pm, packages((i & 1) == 0 ? replacement : first));
        for (Thread reader : readers) { reader.join(10000); check(!reader.isAlive(), "Concurrent reader did not retire"); }
        if (failure.get() != null) throw new AssertionError("Concurrent publication failed", failure.get());
    }

    public static void benchmark(boolean indexed) throws Exception {
        for (int count : new int[]{1, 50, 500, 1000}) {
            CountedPackages packages = new CountedPackages();
            for (int i = 0; i < count; i++) {
                ApkPackage pkg = single("fixture.intent.p" + i, "fixture.action." + i, 20000 + i, true);
                packages.put(pkg.packageName, pkg);
            }
            ApkPackage target = packages.get("fixture.intent.p" + (count - 1));
            for (Kind kind : Kind.values()) {
                PackageManagerService pm = pm(packages);
                Intent hit = new Intent("fixture.action." + (count - 1));
                visits.set(0); reads.set(0); query(pm, kind, hit, null, 0);
                int coldVisits = visits.get(), coldReads = reads.get();
                int[][] work = new int[4][2];
                Intent[] intents = {hit, new Intent("fixture.action.missing"),
                        new Intent("no.filter").setComponent(new ComponentName(target.packageName, target.packageName + "." + kind)),
                        new Intent(hit).setPackage(target.packageName)};
                for (int scenario = 0; scenario < intents.length; scenario++) {
                    visits.set(0); reads.set(0);
                    for (int q = 0; q < 32; q++) {
                        List<ResolveInfo> results = query(pm, kind, intents[scenario], null, 0);
                        if (scenario != 1) check(results.size() == 1 && info(results.get(0)).applicationInfo.uid == 20000 + count - 1,
                                "Measured query changed its result");
                        else check(results.isEmpty() || kind == Kind.SERVICE && results.size() == 1
                                && info(results.get(0)).packageName.equals(GmsPackageSupport.PACKAGE_GMS), "Miss changed fallback behavior");
                    }
                    work[scenario][0] = visits.get(); work[scenario][1] = reads.get();
                }
                System.out.println("INTENT_WORK packages=" + count + " kind=" + kind + " queries=32 cold_visits=" + coldVisits
                        + " cold_reads=" + coldReads + " hit=" + Arrays.toString(work[0]) + " miss=" + Arrays.toString(work[1])
                        + " explicit=" + Arrays.toString(work[2]) + " package=" + Arrays.toString(work[3]));
                if (indexed) {
                    for (int[] row : work) check(row[0] == 0, "Warm query traversed the package registry");
                    check(work[1][1] == 0 && work[0][1] <= 96 && work[2][1] <= 96 && work[3][1] <= 96,
                            "Query read unrelated installed metadata");
                }
            }
        }
    }
    public static void main(String[] args) throws Exception { contracts(true); benchmark(true); }
}
