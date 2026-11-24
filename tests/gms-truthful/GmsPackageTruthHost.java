/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.UserHandle;

import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PackageUids;

import java.lang.reflect.Field;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import sun.misc.Unsafe;

/**
 * Issue #335, package path: Play services and the Play Store are reported only when
 * they are installed. Unchanged PackageManagerService and Android's public
 * ApplicationPackageManager wrapper; boundaries are constructed manifests (as the
 * package visibility host does) and the framework permission catalog.
 *
 * <p>Android contract (android-16.0.0_r4): a package that is not installed is unknown to
 * every query: ApplicationPackageManager.getPackageInfo/getApplicationInfo throw
 * NameNotFoundException when IPackageManager answers null, getInstalledPackages lists
 * only installed packages, queryIntentServices finds only declared components, and
 * get{Application,Component}EnabledSetting throw IllegalArgumentException.
 */
public final class GmsPackageTruthHost {
    static final String GMS = "com.google.android.gms", VENDING = "com.android.vending";
    static final String APP = "fixture.gms.client", AUTH = "com.google.android.gms.auth.service.START";
    static final Unsafe U;
    static int checks;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); U = (Unsafe)f.get(null); }
        catch (ReflectiveOperationException e) { throw new ExceptionInInitializerError(e); }
    }
    interface Call { void run() throws Exception; }
    static void check(boolean v, String why) { if (!v) throw new AssertionError(why); checks++; }
    static void expect(Class<? extends Throwable> type, Call call, String why) throws Exception {
        try { call.run(); }
        catch (Throwable t) {
            if (!type.isInstance(t)) throw new AssertionError(why + ": " + t, t);
            checks++; return;
        }
        throw new AssertionError(why + ": no exception");
    }
    static Object get(Object o, String n) throws Exception { Field f = o.getClass().getDeclaredField(n); f.setAccessible(true); return f.get(o); }
    static void set(Object o, String n, Object v) throws Exception { Field f = o.getClass().getDeclaredField(n); f.setAccessible(true); f.set(o, v); }

    static ApkPackage record(String name, long versionCode, ParsedServiceImpl... services) throws Exception {
        PackageImpl p = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        p.setTargetSdkVersion(36); p.setEnabled(true); p.setVersionCode((int)versionCode);
        for (ParsedServiceImpl s : services) p.addService(s);
        p.hideAsFinal();
        ApkPackage r = (ApkPackage)U.allocateInstance(ApkPackage.class);
        set(r, "packageName", name); set(r, "parsed", p); set(r, "mUid", PackageUids.uidFor(name));
        set(r, "apkPath", "/fixture/" + name + "/base.apk");
        return r;
    }
    static ParsedServiceImpl service(String pkg, String name, String action) {
        ParsedServiceImpl s = new ParsedServiceImpl();
        s.setPackageName(pkg); s.setName(name); s.setEnabled(true); s.setExported(true); s.setProcessName(pkg);
        ParsedIntentInfoImpl info = new ParsedIntentInfoImpl(); info.getIntentFilter().addAction(action);
        s.addIntent(info);
        return s;
    }
    static PackageManagerService pms(ApkPackage... records) throws Exception {
        // As PackageVisibilityContractTest: the installer constructor creates native Binder
        // objects; keep the actual package map, PermissionPolicy and query implementation.
        PackageManagerService pm = (PackageManagerService)U.allocateInstance(PackageManagerService.class);
        Map<String, ApkPackage> installed = new LinkedHashMap<>();
        for (ApkPackage r : records) installed.put(r.packageName, r);
        set(pm, "mPackages", installed); set(pm, "mRemoving", new LinkedHashMap<>());
        var k = Class.forName("com.gesher.server.PermissionPolicy").getDeclaredConstructor(PackageManagerService.class);
        k.setAccessible(true); set(pm, "mPermissions", k.newInstance(pm));
        Object catalog = get(pm.permissions(), "mCatalog");
        set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
        return pm;
    }
    static PackageManager client(IPackageManager service) throws Exception {
        Object context = U.allocateInstance(Class.forName("android.app.ContextImpl"));
        set(context, "mUser", UserHandle.of(0));
        PackageManager m = (PackageManager)U.allocateInstance(Class.forName("android.app.ApplicationPackageManager"));
        set(m, "mContext", context); set(m, "mPM", service); return m;
    }
    @SuppressWarnings("unchecked")
    static List<PackageInfo> installed(PackageManagerService pm, long flags) throws Exception {
        return ((android.content.pm.ParceledListSlice<PackageInfo>)pm.getInstalledPackages(flags, 0)).getList();
    }
    @SuppressWarnings("unchecked")
    static List<ApplicationInfo> applications(PackageManagerService pm) throws Exception {
        return ((android.content.pm.ParceledListSlice<ApplicationInfo>)pm.getInstalledApplications(0, 0)).getList();
    }
    @SuppressWarnings("unchecked")
    static List<ResolveInfo> services(PackageManagerService pm, Intent intent) throws Exception {
        return ((android.content.pm.ParceledListSlice<ResolveInfo>)pm.queryIntentServices(intent, null, 0, 0)).getList();
    }
    static boolean listed(List<PackageInfo> list, String name) {
        for (PackageInfo i : list) if (name.equals(i.packageName)) return true;
        return false;
    }

    /** A clean device without microG: nothing claims Play services or the Play Store. */
    static void withoutMicrog() throws Exception {
        PackageManagerService pm = pms(record(APP, 1));
        PackageManager client = client(pm);
        for (String name : new String[]{GMS, VENDING}) {
            long flags = PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SERVICES
                    | PackageManager.GET_RECEIVERS;
            check(pm.getPackageInfo(name, flags, 0) == null, name + ": IPackageManager reported an absent package");
            expect(PackageManager.NameNotFoundException.class, () -> client.getPackageInfo(name, (int)flags),
                    name + ": public getPackageInfo found an absent package");
            expect(PackageManager.NameNotFoundException.class, () -> client.getApplicationInfo(name, 0),
                    name + ": public getApplicationInfo found an absent package");
            check(!pm.isPackageAvailable(name, 0), name + ": reported available");
            check(!listed(installed(pm, flags), name), name + ": listed as installed");
            check(applications(pm).stream().noneMatch(a -> name.equals(a.packageName)),
                    name + ": listed among installed applications");
            expect(IllegalArgumentException.class, () -> pm.getApplicationEnabledSetting(name, 0),
                    name + ": enabled state of an unknown package");
            expect(IllegalArgumentException.class, () -> pm.getComponentEnabledSetting(
                    new android.content.ComponentName(name, name + ".Any"), 0),
                    name + ": component state of an unknown package");
            check(pm.getInstallSourceInfo(name, 0) == null, name + ": install source of an absent package");
        }
        // Client libraries bind Play services by action and package: nothing serves it.
        for (Intent intent : new Intent[]{new Intent(AUTH).setPackage(GMS), new Intent(AUTH),
                new Intent("com.google.android.c2dm.intent.REGISTER").setPackage(GMS)}) {
            check(services(pm, intent).isEmpty(), "service resolved without Play services: " + intent);
            check(pm.resolveService(intent, null, 0, 0) == null, "resolveService answered: " + intent);
        }
        check(listed(installed(pm, 0), APP), "the installed client is still listed");
    }

    /** Real microG installed: its own record, components and version answer. */
    static void withMicrog() throws Exception {
        ApkPackage gms = record(GMS, 240913000L, service(GMS, "org.microg.gms.auth.AuthManagerServiceImpl", AUTH));
        PackageManagerService pm = pms(record(APP, 1), gms);
        PackageManager client = client(pm);
        PackageInfo info = client.getPackageInfo(GMS, 0);
        check(info.getLongVersionCode() == 240913000L, "microG's own version replaced: " + info.getLongVersionCode());
        check(listed(installed(pm, 0), GMS), "installed microG not listed");
        List<ResolveInfo> bound = services(pm, new Intent(AUTH).setPackage(GMS));
        check(bound.size() == 1 && "org.microg.gms.auth.AuthManagerServiceImpl".equals(bound.get(0).serviceInfo.name),
                "auth service did not route to microG's component: " + bound);
        check(services(pm, new Intent("fixture.undeclared").setPackage(GMS)).isEmpty(),
                "an undeclared microG action resolved");
        // The Play Store is a separate package: absent unless installed.
        expect(PackageManager.NameNotFoundException.class, () -> client.getPackageInfo(VENDING, 0),
                "Play Store reported beside microG");
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        withoutMicrog();
        withMicrog();
        System.out.println("GMS package truth host passed: " + checks + " checks");
        System.out.println("Actual PMS and ApplicationPackageManager; constructed manifests, framework-catalog boundary. No ART/Binder transport claim.");
    }
}
