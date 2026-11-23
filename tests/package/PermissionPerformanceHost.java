/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.lang.reflect.Field;
import java.security.MessageDigest;
import java.security.MessageDigestSpi;
import java.security.Provider;
import java.security.Security;
import java.util.AbstractCollection;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.WeakHashMap;
import sun.misc.Unsafe;

/** Operation counts in production policy, with only parsed APK/native boundaries constructed.
 * No timing threshold: these counts are independent of host load and are not Windows timings. */
public final class PermissionPerformanceHost {
    private static final String NORMAL = "android.permission.VIBRATE";
    private static final String RUNTIME = "android.permission.CAMERA";
    private static final Unsafe UNSAFE;
    private static long digestCalls, packageVisits;
    private static int checks;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            UNSAFE = (Unsafe)field.get(null);
        } catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    public static final class CountingDigest extends MessageDigestSpi {
        private final MessageDigest real;
        public CountingDigest() {
            try { real = MessageDigest.getInstance("SHA-256", "SUN"); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Override protected void engineUpdate(byte value) { real.update(value); }
        @Override protected void engineUpdate(byte[] value, int offset, int length) { real.update(value, offset, length); }
        @Override protected byte[] engineDigest() { digestCalls++; return real.digest(); }
        @Override protected void engineReset() { real.reset(); }
    }
    private static final class CountingPackages extends LinkedHashMap<String, ApkPackage> {
        @Override public Collection<ApkPackage> values() {
            Collection<ApkPackage> values = super.values();
            return new AbstractCollection<>() {
                @Override public int size() { return values.size(); }
                @Override public Iterator<ApkPackage> iterator() {
                    Iterator<ApkPackage> iterator = values.iterator();
                    return new Iterator<>() {
                        @Override public boolean hasNext() { return iterator.hasNext(); }
                        @Override public ApkPackage next() { packageVisits++; return iterator.next(); }
                    };
                }
            };
        }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message); checks++;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name); field.setAccessible(true); field.set(target, value);
    }
    private static Object get(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(target);
    }
    private static ApkPackage pkg(String name, int uid, byte signer, String... requested) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String permission : requested) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(pkg, "packageName", name); set(pkg, "parsed", parsed); set(pkg, "mUid", uid);
        // This fixture starts after verified admission. The independent signing/UID
        // suites exercise real signed, tampered and rotated APKs before admission.
        set(pkg, "mSigningIdentityChecked", true);
        set(pkg, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{signer})});
        return pkg;
    }
    private static PermissionInfo definition(String name, int protection) {
        PermissionInfo info = new PermissionInfo(); info.name = name; info.packageName = "android";
        info.protectionLevel = protection; return info;
    }
    private static final class Fixture {
        final PackageManagerService pm;
        final PermissionCatalog catalog;
        final PermissionPolicy policy;
        final Properties decisions = new Properties();
        final Map<ApkPackage, Boolean> retired = new WeakHashMap<>();
        final List<Properties> persisted = new ArrayList<>();
        final PolicyStateWriter<Properties> writer = new PolicyStateWriter<>("Permission performance fixture", persisted::add);
        @SuppressWarnings("unchecked") Fixture(CountingPackages packages) throws Exception {
            pm = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", packages);
            catalog = new PermissionCatalog(pm);
            Map<String, PermissionInfo> framework = (Map<String, PermissionInfo>)get(catalog, "mFramework");
            framework.put(NORMAL, definition(NORMAL, PermissionInfo.PROTECTION_NORMAL));
            framework.put(RUNTIME, definition(RUNTIME, PermissionInfo.PROTECTION_DANGEROUS));
            set(catalog, "mLoaded", true);
            policy = (PermissionPolicy)UNSAFE.allocateInstance(PermissionPolicy.class);
            set(policy, "mPm", pm); set(policy, "mCatalog", catalog);
            set(policy, "mDecisions", decisions); set(policy, "mForgottenPackages", retired);
            set(policy, "mWriter", writer);
            set(pm, "mPermissions", policy);
        }
        void publish(CountingPackages packages) throws Exception {
            // Execute the same publication/invalidation method used by boot,
            // package replacement and removal, not a test-side cache clear.
            var method = PackageManagerService.class.getDeclaredMethod("publishPackages", Map.class);
            method.setAccessible(true); method.invoke(pm, packages);
        }
    }
    private static CountingPackages packages(ApkPackage... values) {
        CountingPackages packages = new CountingPackages();
        for (ApkPackage pkg : values) packages.put(pkg.packageName, pkg);
        return packages;
    }
    private static void normalChecks() throws Exception {
        ApkPackage owner = pkg("fixture.owner", 12000, (byte)1, NORMAL);
        ApkPackage shared = pkg("fixture.shared", 12000, (byte)1);
        CountingPackages packages = packages(owner, shared);
        for (int i = 0; i < 18; i++) {
            ApkPackage unrelated = pkg("fixture.unrelated" + i, 13000 + i, (byte)2);
            packages.put(unrelated.packageName, unrelated);
        }
        Fixture fixture = new Fixture(packages);
        check(fixture.policy.granted(NORMAL, shared.packageName), "A shared UID inherits its requesting member's normal permission");
        digestCalls = 0; packageVisits = 0;
        for (int i = 0; i < 10000; i++) {
            if (!fixture.policy.granted(NORMAL, shared.packageName)) throw new AssertionError("Normal grant changed");
        }
        System.out.println("normal: 10000 checks, 20 packages, " + digestCalls
                + " certificate hashes, " + packageVisits + " full-map package visits");
        check(digestCalls == 0, "Warm normal grants must not hash signer certificates; got " + digestCalls);
        check(packageVisits == 0, "Warm normal grants must not scan installed packages; got " + packageVisits);
        check(!fixture.policy.granted(NORMAL, "fixture.unrelated0"), "A distinct nonrequesting UID is denied");
        check(!fixture.policy.granted("android.permission.UNKNOWN", owner.packageName), "Unknown definitions remain denied");
        int flags = PackageManager.FLAG_PERMISSION_USER_SET | PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        fixture.decisions.setProperty("flags:12000:" + NORMAL, Integer.toString(flags));
        fixture.decisions.setProperty("uid:12000:" + NORMAL, "denied");
        check(fixture.policy.granted(NORMAL, shared.packageName), "Runtime decisions do not revoke a manifest normal grant");
        check(fixture.policy.getPermissionFlags(shared.packageName, NORMAL, "default:0", 0) == flags,
                "The boolean fast path must not erase observable permission flags");
        fixture.retired.put(owner, true);
        check(!fixture.policy.granted(NORMAL, shared.packageName), "A retired requester is excluded even before map publication");
        fixture.retired.remove(owner);
        check(fixture.policy.granted(NORMAL, shared.packageName), "Restoring the same generation restores UID membership");
        ApkPackage replacement = pkg(owner.packageName, 14000, (byte)3, NORMAL);
        fixture.publish(packages(replacement, shared));
        check(!fixture.policy.granted(NORMAL, shared.packageName), "Replacement moving UID invalidates the old UID membership");
        check(fixture.policy.granted(NORMAL, replacement.packageName), "The replacement is indexed under its admitted UID");
        fixture.publish(packages(shared));
        check(!fixture.policy.granted(NORMAL, owner.packageName), "Removed packages cannot retain a cached grant");
        fixture.retired.put(owner, true);
        ApkPackage reinstalled = pkg(owner.packageName, 12000, (byte)1, NORMAL);
        fixture.publish(packages(reinstalled, shared));
        check(fixture.policy.granted(NORMAL, shared.packageName), "Retirement is tied to an APK generation, not its name or UID");
    }
    private static void signerChecks() throws Exception {
        ApkPackage owner = pkg("fixture.signer", 15000, (byte)1, RUNTIME);
        digestCalls = 0;
        String signer = owner.verifiedSignerSha256();
        for (int i = 0; i < 10000; i++) {
            if (!signer.equals(owner.verifiedSignerSha256())) throw new AssertionError("Cached fingerprint changed");
        }
        check(digestCalls == 1, "One verified APK generation must hash its signer once; got " + digestCalls);
        Fixture fixture = new Fixture(packages(owner));
        fixture.decisions.setProperty(owner.packageName + ":" + signer + ":" + RUNTIME, "granted");
        check(fixture.policy.granted(RUNTIME, owner.packageName), "Existing signer-keyed legacy decisions are honored");
        ApkPackage replacement = pkg(owner.packageName, owner.uid(), (byte)2, RUNTIME);
        fixture.publish(packages(replacement));
        check(!fixture.policy.granted(RUNTIME, replacement.packageName), "A different verified signer cannot inherit a legacy grant");
        check(!signer.equals(replacement.verifiedSignerSha256()) && digestCalls == 2,
                "Replacement computes its own fingerprint, without a package-name or timestamp cache");
        ApkPackage unsigned = pkg("fixture.unsigned", 15001, (byte)1);
        set(unsigned, "mVerifiedSignatures", null);
        check(unsigned.verifiedSignerSha256() == null, "An unverified identity cannot acquire a cached fingerprint");
        set(unsigned, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{1}), new Signature(new byte[]{2})});
        check(unsigned.verifiedSignerSha256() == null, "A multisigner APK has no single-signer legacy fingerprint");
        System.out.println("signer: 10001 lookups, 1 certificate hash per verified APK generation; replacement grant denied");
    }
    private static void declares(ApkPackage pkg, String name, int protection) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(pkg.packageName, "/fixture/" + pkg.packageName + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String requested : pkg.parsed.getRequestedPermissions()) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(requested, 0, Set.of()));
        }
        for (var declared : pkg.parsed.getPermissions()) parsed.addPermission(declared);
        ParsedPermissionImpl permission = new ParsedPermissionImpl();
        permission.setName(name); permission.setPackageName(pkg.packageName); permission.setProtectionLevel(protection);
        parsed.addPermission(permission); parsed.hideAsFinal(); set(pkg, "parsed", parsed);
    }
    private static void startupChecks() throws Exception {
        for (int count : new int[]{25, 100}) {
            CountingPackages packages = new CountingPackages();
            for (int i = 0; i < count; i++) {
                ApkPackage pkg = pkg("fixture.startup" + i, 16000 + i, (byte)1);
                declares(pkg, pkg.packageName + ".CUSTOM", PermissionInfo.PROTECTION_NORMAL);
                packages.put(pkg.packageName, pkg);
            }
            Fixture fixture = new Fixture(packages);
            packageVisits = 0;
            fixture.policy.initializePackages();
            check(packageVisits == 3L * count, "Initial catalogue/UID reconciliation must visit packages three times, not per UID/permission: " + packageVisits);
            System.out.println("startup: " + count + " packages/custom permissions, " + packageVisits + " full-map package visits");
            packageVisits = 0;
            fixture.policy.initializePackages();
            check(packageVisits == count, "Reconciliation of the same publication reuses the UID and declaration indexes: " + packageVisits);
        }
        CountingPackages packages = new CountingPackages();
        for (int i = 0; i < 100; i++) {
            String name = "fixture.runtime" + i;
            ApkPackage pkg = pkg(name, 18000 + i, (byte)1, name + ".CUSTOM");
            declares(pkg, name + ".CUSTOM", PermissionInfo.PROTECTION_DANGEROUS);
            packages.put(name, pkg);
        }
        Fixture fixture = new Fixture(packages);
        packageVisits = 0;
        fixture.policy.initializePackages();
        check(packageVisits == 300, "Requested runtime definitions reuse the same catalogue snapshot: " + packageVisits);
        check(fixture.decisions.size() == 300, "Boot still reconciles decision, legacy basis and flags for all 100 requesting UIDs");
        check(fixture.writer.flush() && fixture.persisted.size() == 1 && fixture.persisted.get(0).equals(fixture.decisions),
                "Boot publishes the complete reconciled state to the real policy writer");
        System.out.println("startup: 100 requested runtime definitions, 300 full-map package visits, 100 complete UID states");
    }
    private static void catalogChecks() throws Exception {
        ApkPackage first = pkg("fixture.first", 17000, (byte)1);
        ApkPackage same = pkg("fixture.same", 17001, (byte)1);
        ApkPackage different = pkg("fixture.different", 17002, (byte)2);
        declares(first, "fixture.SAME", PermissionInfo.PROTECTION_NORMAL);
        declares(first, "fixture.CONFLICT", PermissionInfo.PROTECTION_SIGNATURE);
        declares(first, NORMAL, PermissionInfo.PROTECTION_SIGNATURE);
        declares(first, "android.permission.UNDECLARED", PermissionInfo.PROTECTION_NORMAL);
        declares(different, "fixture.CONFLICT", PermissionInfo.PROTECTION_NORMAL);
        declares(same, "fixture.SAME", PermissionInfo.PROTECTION_DANGEROUS);
        declares(same, "fixture.CONFLICT", PermissionInfo.PROTECTION_SIGNATURE);
        declares(same, "fixture.LAST", PermissionInfo.PROTECTION_NORMAL);
        Fixture fixture = new Fixture(packages(first, different, same));
        Map<String, PermissionInfo> snapshot = fixture.catalog.permissions();
        check(snapshot.get("fixture.CONFLICT") == null && fixture.catalog.get("fixture.CONFLICT") == null,
                "A conflicting signer remains rejected even after a later matching declaration");
        check(snapshot.get("android.permission.UNDECLARED") == null, "Reserved platform names cannot be supplied by apps");
        check(snapshot.get(NORMAL).protectionLevel == PermissionInfo.PROTECTION_NORMAL, "The platform definition wins over app declarations");
        check(snapshot.get("fixture.SAME").packageName.equals(same.packageName)
                        && snapshot.get("fixture.SAME").protectionLevel == PermissionInfo.PROTECTION_DANGEROUS,
                "Same-signer duplicates preserve the last definition");
        check(new ArrayList<>(snapshot.keySet()).equals(List.of(NORMAL, RUNTIME, "fixture.SAME", "fixture.LAST")),
                "Catalogue order is stable at the first surviving declaration");
        for (var entry : snapshot.entrySet()) {
            PermissionInfo direct = fixture.catalog.get(entry.getKey());
            check(direct != null && entry.getValue().packageName.equals(direct.packageName)
                            && entry.getValue().protectionLevel == direct.protectionLevel,
                    "Snapshot and individual permission definitions agree");
        }
        packageVisits = 0;
        for (int i = 0; i < 10000; i++) {
            PermissionInfo declared = fixture.catalog.get("fixture.LAST");
            if (declared == null || !declared.packageName.equals(same.packageName)) throw new AssertionError("Declared definition changed");
            declared.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;  // callers receive private copies
        }
        System.out.println("catalog: 10000 app-declared definition lookups, " + packageVisits + " full-map package visits");
        check(packageVisits == 0, "Warm app-declared definitions must not scan installed packages; got " + packageVisits);
        check(fixture.catalog.get("fixture.LAST").protectionLevel == PermissionInfo.PROTECTION_NORMAL,
                "A caller's copy cannot change the published definition");
        check(fixture.catalog.get("fixture.MISSING") == null && packageVisits == 0, "Undeclared names remain absent without a scan");
        fixture.publish(packages(first));
        check(fixture.catalog.get("fixture.LAST") == null, "A removed declaration is not served from the previous publication");
        check(fixture.catalog.permissions().get("fixture.CONFLICT") != null,
                "Removing the conflicting declaration is reflected by the next catalogue snapshot");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Provider provider = new Provider("GesherCountSHA256", "1.0", "Delegating operation-count fixture") { };
        provider.put("MessageDigest.SHA-256", CountingDigest.class.getName()); Security.insertProviderAt(provider, 1);
        try {
            String selected = args.length > 1 ? args[1] : "all";
            if (selected.equals("all") || selected.equals("normal")) normalChecks();
            if (selected.equals("all") || selected.equals("signer")) signerChecks();
            if (selected.equals("all") || selected.equals("startup")) startupChecks();
            if (selected.equals("all") || selected.equals("catalog")) catalogChecks();
            System.out.println("Permission performance contracts passed: " + checks + " checks (" + selected + ")");
        } finally { Security.removeProvider(provider.getName()); }
    }
}
