/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
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
import com.gesher.server.WindowsUriHandlers;
import java.lang.reflect.*;
import java.nio.file.Files;
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
    /** Change real installed state at a deterministic point in full framework generation. */
    private static final class TransitionPackage extends PackageImpl {
        final Kind family;
        Runnable transition;
        int familyReads;
        TransitionPackage(String name, Kind family) {
            super(name, "/fixture/" + name + "/base.apk", "/fixture/" + name + "/base.apk", null, false, null);
            this.family = family;
        }
        private <T> List<T> observe(List<T> values, Kind kind) {
            if (transition != null && family == kind && ++familyReads == values.size() + 2) {
                Runnable action = transition; transition = null; action.run();
            }
            return values;
        }
        @Override public List<ParsedActivity> getActivities() { return observe(super.getActivities(), Kind.ACTIVITY); }
        @Override public List<ParsedActivity> getReceivers() { return observe(super.getReceivers(), Kind.RECEIVER); }
        @Override public List<ParsedService> getServices() { return observe(super.getServices(), Kind.SERVICE); }
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
        // Unsafe skips field initializers; this manifest-index fixture has no Windows associations.
        try {
            field(PackageManagerService.class, "mUriHandlers").set(result, new WindowsUriHandlers(null));
        } catch (NoSuchFieldException beforeUriHandlers) {
            // The documented --source-ref comparison predates this optional host package.
        }
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

    private static ComponentInfo individual(ApkPackage pkg, Kind kind, String name, long flags) {
        return switch (kind) {
            case ACTIVITY -> pkg.activityInfo(name, flags);
            case RECEIVER -> pkg.receiverInfo(name, flags);
            case SERVICE -> pkg.serviceInfo(name, flags);
            case PROVIDER -> throw new AssertionError("Providers are a separate registration contract");
        };
    }

    /** Compare real generated values, including fields added to the framework classes. */
    private static void sameMetadata(Object expected, Object actual, String path) throws Exception {
        if (expected == actual) return;
        check(expected != null && actual != null && expected.getClass() == actual.getClass(), path);
        if (expected.getClass().isArray()) {
            check(Arrays.deepEquals(new Object[]{expected}, new Object[]{actual}), path); return;
        }
        if (expected instanceof List<?> left && actual instanceof List<?> right) {
            check(left.size() == right.size(), path + " size");
            for (int i = 0; i < left.size(); i++) sameMetadata(left.get(i), right.get(i), path + "[" + i + "]");
            return;
        }
        if (!expected.getClass().getName().startsWith("android.content.pm.")) {
            check(expected.equals(actual), path + " value"); return;
        }
        for (Class<?> type = expected.getClass(); type != Object.class; type = type.getSuperclass()) {
            for (Field f : type.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers()) || f.isSynthetic()) continue;
                f.setAccessible(true);
                // Each independently generated ApplicationInfo stamps its own
                // uptime; compare validity rather than different construction instants.
                if (type == ApplicationInfo.class && f.getName().equals("createTimestamp")) {
                    long now = android.os.SystemClock.uptimeMillis();
                    check(f.getLong(expected) >= 0 && f.getLong(expected) <= now
                            && f.getLong(actual) >= 0 && f.getLong(actual) <= now, path + ".createTimestamp");
                    continue;
                }
                sameMetadata(f.get(expected), f.get(actual), path + "." + f.getName());
            }
        }
    }

    /** Full production generation and public single-component getters form the value control. */
    public static void metadataContracts() throws Exception {
        int before = checks;
        for (Kind kind : new Kind[]{Kind.ACTIVITY, Kind.RECEIVER, Kind.SERVICE}) {
            for (int count : new int[]{7, 8}) {
                PackageImpl parsed = manifest("fixture.metadata." + kind.name().toLowerCase(Locale.ROOT) + ".p" + count, false);
                ParsedIntentInfoImpl first = filter(ACTION, true); first.getIntentFilter().setPriority(7);
                ParsedIntentInfoImpl later = filter(ACTION, true); later.getIntentFilter().setPriority(99);
                declaration(parsed, kind, "First", true, false, first, later);
                declaration(parsed, kind, "Duplicate", false, false, filter(ACTION, true));
                declaration(parsed, kind, "Duplicate", true, true, filter(ACTION, true));
                declaration(parsed, kind, "Duplicate", true, false, filter(ACTION, true));
                declaration(parsed, kind, "Other", true, false, filter(ACTION, false));
                declaration(parsed, kind, "Explicit", true, false);
                declaration(parsed, kind, "Unrelated", true, false, filter("other.action", true));
                if (count == 8) declaration(parsed, kind, "BatchThreshold", true, false, filter("other.action", true));
                if (kind != Kind.SERVICE) {
                    List<ParsedActivity> activities = kind == Kind.ACTIVITY ? parsed.getActivities() : parsed.getReceivers();
                    for (int i = 0; i < activities.size(); i++) {
                        ParsedActivityImpl activity = (ParsedActivityImpl)activities.get(i);
                        activity.setTheme(101 + i);
                        activity.setKnownActivityEmbeddingCerts(Set.of("ab" + i));
                        activity.setMaxAspectRatio(1.5f + i);
                        activity.setMinAspectRatio(0.5f + i);
                    }
                }
                ApkPackage pkg = apk(parsed, 12451);
                PackageManagerService pm = pm(packages(pkg));
                Intent intent = new Intent(ACTION).setPackage(pkg.packageName);
                long[] flags = {0, PackageManager.GET_META_DATA | PackageManager.GET_RESOLVED_FILTER,
                        PackageManager.MATCH_DEFAULT_ONLY, PackageManager.MATCH_DIRECT_BOOT_AWARE,
                        PackageManager.MATCH_DIRECT_BOOT_UNAWARE, PackageManager.MATCH_DISABLED_COMPONENTS,
                        PackageManager.MATCH_DISABLED_COMPONENTS | PackageManager.MATCH_DIRECT_BOOT_UNAWARE,
                        PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES
                                | PackageManager.GET_ACTIVITIES | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES};
                for (long flag : flags) {
                    List<ResolveInfo> direct = declared(pkg, kind, intent, null, flag);
                    int expected = (flag & PackageManager.MATCH_DIRECT_BOOT_AWARE) != 0 ? 3
                            : (flag & PackageManager.MATCH_DEFAULT_ONLY) != 0 ? 4 : 5;
                    check(direct.size() == expected, "Filtered duplicate result count: " + kind + " flags=" + flag);
                    check(info(direct.get(0)).name.endsWith(expected == 3 ? ".Duplicate" : ".First"),
                            "Declaration result order changed");
                    if (expected != 3) check(direct.get(0).priority == 7, "Later matching filter replaced the first");
                    for (ResolveInfo resolved : direct) {
                        ComponentInfo component = info(resolved);
                        sameMetadata(individual(pkg, kind, component.name, flag), component, kind + " " + component.name);
                    }
                    List<ResolveInfo> publicResults = query(pm, kind, intent, null, flag);
                    check(describe(publicResults).equals(describe(direct)), "Public PM changed family order or flags");
                }
                List<ResolveInfo> duplicates = declared(pkg, kind, intent, null, PackageManager.GET_RESOLVED_FILTER);
                ComponentInfo one = info(duplicates.get(1)), two = info(duplicates.get(2));
                check(one != two && one.applicationInfo != two.applicationInfo, "Duplicate results share returned metadata");
                one.applicationInfo.uid = -1; one.name = "caller.changed";
                check(two.applicationInfo.uid == 12451 && two.name.endsWith(".Duplicate"), "Caller mutation crossed results");
                duplicates.get(1).filter.addAction("caller.injected");
                check(!duplicates.get(2).filter.hasAction("caller.injected"), "Resolved filters share caller mutations");
                if (one instanceof ActivityInfo a && two instanceof ActivityInfo b) {
                    a.getKnownActivityEmbeddingCerts().clear();
                    check(!b.getKnownActivityEmbeddingCerts().isEmpty(), "Embedding certificate sets share caller mutations");
                }
                List<ResolveInfo> fresh = declared(pkg, kind, intent, null, PackageManager.GET_RESOLVED_FILTER);
                check(info(fresh.get(1)).applicationInfo.uid == 12451 && info(fresh.get(1)).name.endsWith(".Duplicate")
                        && !fresh.get(1).filter.hasAction("caller.injected"), "A query retained caller mutations");
                if (info(fresh.get(1)) instanceof ActivityInfo a) check(!a.getKnownActivityEmbeddingCerts().isEmpty(),
                        "A query retained embedding certificate mutations");

                String duplicate = pkg.packageName + ".Duplicate";
                for (int state : new int[]{PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                        PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT}) {
                    invoke(method(ApkPackage.class, "setComponentEnabledState", String.class, int.class), pkg, duplicate, state);
                    List<ResolveInfo> result = declared(pkg, kind, intent, null, 0);
                    check(result.size() == (state == PackageManager.COMPONENT_ENABLED_STATE_DISABLED ? 2 : 5),
                            "Live installed enabled state was retained");
                    for (ResolveInfo resolved : result) sameMetadata(individual(pkg, kind, info(resolved).name, 0),
                            info(resolved), "live enabled metadata");
                }
                check(declared(pkg, kind, new Intent("absent"), null, 0).isEmpty(), "Empty match path changed");
                Intent explicit = new Intent("absent").setComponent(new ComponentName(pkg.packageName, pkg.packageName + ".Explicit"));
                check(declared(pkg, kind, explicit, null, 0).size() == 1, "Explicit unfiltered component disappeared");
                libraryIsolation(pkg, kind, intent);
            }
        }
        System.out.println("Component query metadata contracts passed: " + (checks - before) + " checks");
    }

    /** Constructed immutable installed graph, actual graph-to-ApplicationInfo generation. */
    private static void libraryIsolation(ApkPackage pkg, Kind kind, Intent intent) throws Exception {
        java.nio.file.Path directory = Files.createTempDirectory("gesher-component-libraries-");
        try {
            java.nio.file.Path library = directory.resolve("fixture.jar"); Files.write(library, new byte[]{1, 2, 3});
            SharedLibraryGraph.Node dependency = new SharedLibraryGraph.Node("dependency", library.toFile(), List.of());
            SharedLibraryGraph.Node first = new SharedLibraryGraph.Node("first", library.toFile(), List.of(dependency));
            SharedLibraryGraph.Node second = new SharedLibraryGraph.Node("second", library.toFile(), List.of(dependency));
            field(ApkPackage.class, "sharedLibraries").set(pkg, new SharedLibraryGraph(List.of(first, second), Set.of()));
            List<ResolveInfo> results = declared(pkg, kind, intent, null, PackageManager.GET_SHARED_LIBRARY_FILES);
            ApplicationInfo a = info(results.get(0)).applicationInfo, b = info(results.get(1)).applicationInfo;
            check(a.sharedLibraryFiles != b.sharedLibraryFiles && a.sharedLibraryInfos != b.sharedLibraryInfos,
                    "Application library containers share returned ownership");
            check(a.sharedLibraryInfos.get(0) != b.sharedLibraryInfos.get(0)
                    && a.sharedLibraryInfos.get(0).getDependencies().get(0)
                        != b.sharedLibraryInfos.get(0).getDependencies().get(0), "Library graphs share returned nodes");
            check(a.sharedLibraryInfos.get(0).getDependencies().get(0)
                    == a.sharedLibraryInfos.get(1).getDependencies().get(0), "One graph lost its shared dependency");
            a.sharedLibraryFiles[0] = "caller.changed";
            a.sharedLibraryInfos.get(0).getDependencies().clear(); a.sharedLibraryInfos.clear();
            check(!b.sharedLibraryFiles[0].equals("caller.changed") && b.sharedLibraryInfos.size() == 2
                    && b.sharedLibraryInfos.get(0).getDependencies().size() == 1, "Caller mutation crossed library graphs");
            ApplicationInfo fresh = info(declared(pkg, kind, intent, null, PackageManager.GET_SHARED_LIBRARY_FILES).get(0)).applicationInfo;
            check(fresh.sharedLibraryInfos.size() == 2 && fresh.sharedLibraryInfos.get(0).getDependencies().size() == 1
                    && !fresh.sharedLibraryFiles[0].equals("caller.changed"), "Caller mutated installed library metadata");
        } finally {
            field(ApkPackage.class, "sharedLibraries").set(pkg, SharedLibraryGraph.EMPTY);
            Files.deleteIfExists(directory.resolve("fixture.jar")); Files.deleteIfExists(directory);
        }
    }

    private static ApkPackage componentPackage(String scenario, int count, boolean counted) throws Exception {
        PackageImpl parsed = manifest("fixture.component.work", counted);
        Kind kind = scenario.startsWith("service") ? Kind.SERVICE
                : scenario.startsWith("receiver") ? Kind.RECEIVER : Kind.ACTIVITY;
        for (int i = 0; i < count; i++) {
            boolean hit = !scenario.endsWith("miss") && (!scenario.endsWith("single") || i == 0)
                    && (!scenario.endsWith("sparse") || i < 4);
            declaration(parsed, kind, "Component" + i, !scenario.endsWith("disabled"), false,
                    filter(hit ? ACTION : "other.action", true));
        }
        return apk(parsed, 12452);
    }

    private static Kind componentKind(String scenario) {
        return scenario.startsWith("service") ? Kind.SERVICE
                : scenario.startsWith("receiver") ? Kind.RECEIVER : Kind.ACTIVITY;
    }

    public static void metadataCoherence() throws Exception {
        int before = checks;
        for (Kind kind : new Kind[]{Kind.ACTIVITY, Kind.RECEIVER, Kind.SERVICE}) {
            for (int count : new int[]{1, 2, 7, 8}) {
                for (int setting : new int[]{PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER,
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED}) {
                    TransitionPackage parsed = new TransitionPackage("fixture.metadata.transition", kind);
                    parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.setProcessName(parsed.getPackageName());
                    for (int i = 0; i < count; i++) declaration(parsed, kind, "Component" + i, true, false, filter(ACTION, true));
                    ApkPackage pkg = apk(parsed, 12453);
                    Object enabled = invoke(method(ApkPackage.class, "enabledState"), pkg);
                    Object disabled = invoke(method(enabled.getClass(), "application", int.class), enabled, setting);
                    Field state = field(ApkPackage.class, "mLocalEnabledState");
                    parsed.familyReads = 0;
                    parsed.transition = () -> {
                        try { state.set(pkg, disabled); }
                        catch (IllegalAccessException impossible) { throw new AssertionError(impossible); }
                    };
                    List<ResolveInfo> results = declared(pkg, kind, new Intent(ACTION), null, 0);
                    check(parsed.transition == null && state.get(pkg) == disabled, "Enabled transition was not exercised");
                    // Batched generation captures one state. Direct tiny queries keep
                    // their existing per-result read and stop returning newly disabled
                    // components; each returned component/AI pair must remain coherent.
                    check(results.size() == (count >= 8 ? count : 1), "Enabled transition changed query generation semantics");
                    for (ResolveInfo result : results) check(info(result).enabled && info(result).applicationInfo.enabled
                            && info(result).applicationInfo.enabledSetting == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                            "Returned component and application disagree about the captured enabled state");
                    check(declared(pkg, kind, new Intent(ACTION), null, 0).isEmpty(), "Next query retained stale enabled state");
                    List<ResolveInfo> disabledResults = declared(pkg, kind, new Intent(ACTION), null,
                            PackageManager.MATCH_DISABLED_COMPONENTS);
                    check(disabledResults.size() == count, "MATCH_DISABLED_COMPONENTS lost declarations");
                    for (ResolveInfo result : disabledResults) check(!info(result).applicationInfo.enabled
                            && info(result).applicationInfo.enabledSetting == setting, "Disabled query lost its actual application state");
                    List<ResolveInfo> untilUsed = declared(pkg, kind, new Intent(ACTION), null,
                            PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS);
                    check(untilUsed.size() == (setting == PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED ? count : 0),
                            "Disabled-until-used query flags changed");
                    for (ResolveInfo result : untilUsed) check(info(result).applicationInfo.enabled,
                            "Disabled-until-used metadata did not honor query flags");
                    state.set(pkg, enabled);
                    check(declared(pkg, kind, new Intent(ACTION), null, 0).size() == count, "Reenable retained the previous snapshot");
                }
            }
        }
        System.out.println("Component query enabled-snapshot contracts passed: " + (checks - before) + " checks");
    }

    /** Counts real framework component-list reads; no metadata generator is replaced. */
    public static void metadataWork() throws Exception {
        int count = 64, queries = 8;
        for (Kind kind : new Kind[]{Kind.ACTIVITY, Kind.RECEIVER, Kind.SERVICE}) {
            String scenario = kind.name().toLowerCase(Locale.ROOT) + "-broad";
            ApkPackage pkg = componentPackage(scenario, count, true);
            reads.set(0);
            for (int q = 0; q < queries; q++) {
                List<ResolveInfo> results = declared(pkg, kind, new Intent(ACTION), null, 0);
                check(results.size() == count, "Measured metadata query lost results");
                for (int i = 0; i < count; i++) check(info(results.get(i)).name.equals(pkg.packageName + ".Component" + i),
                        "Measured metadata query changed order");
            }
            int measured = reads.get();
            System.out.println("COMPONENT_METADATA_WORK kind=" + kind + " components=" + count
                    + " queries=" + queries + " component_list_reads=" + measured);
            check(measured <= queries * (count + 2), "Query repeated full-family metadata generation: " + measured);
        }
    }

    /** Uninstrumented manifests for host CPU/allocation measurements of real public queries. */
    public static java.util.function.IntSupplier metadataWorkload(String scenario, int count) throws Exception {
        ApkPackage pkg = componentPackage(scenario, count, false);
        Kind kind = componentKind(scenario);
        Intent intent = new Intent(ACTION);
        long flags = PackageManager.GET_META_DATA | PackageManager.GET_RESOLVED_FILTER;
        int expected = scenario.endsWith("miss") || scenario.endsWith("disabled") ? 0
                : scenario.endsWith("single") ? 1 : scenario.endsWith("sparse") ? Math.min(4, count) : count;
        return () -> {
            List<ResolveInfo> results = declared(pkg, kind, intent, null, flags);
            if (results.size() != expected) throw new AssertionError("Measured metadata result count");
            int checksum = results.size();
            for (ResolveInfo result : results) {
                ComponentInfo component = info(result);
                if (component.applicationInfo.uid != 12452 || !component.name.startsWith(pkg.packageName)
                        || result.filter == null || !result.filter.hasAction(ACTION)) throw new AssertionError("Measured metadata values");
                checksum += component.name.hashCode() + component.applicationInfo.uid + result.match;
            }
            return checksum;
        };
    }

    public static void main(String[] args) throws Exception {
        contracts(true); benchmark(true); metadataContracts(); metadataWork(); metadataCoherence();
    }
}
