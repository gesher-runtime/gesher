/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.UriRelativeFilter;
import android.content.UriRelativeFilterGroup;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.PatternMatcher;
import android.os.RemoteException;

import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.BrowserPackageSupport;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import sun.misc.Unsafe;

/** Actual PMS/framework resolution, plus the real parsed-APK -> AIDL -> public manager path. */
public final class ActivityOptionsContractTest {
    private static final String ACTION = "fixture.options.GENERIC";
    private static final String EXTRA = "fixture.options.SPECIFIC";
    private static final long FILTER = PackageManager.GET_RESOLVED_FILTER;
    private static int checks;
    private static Unsafe unsafe;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void field(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static ParsedIntentInfoImpl filter(String label, int priority, boolean isDefault,
            String... actions) {
        ParsedIntentInfoImpl info = new ParsedIntentInfoImpl();
        info.setNonLocalizedLabel(label);
        info.setLabelRes(71);
        info.setIcon(83);
        info.setHasDefault(isDefault);
        IntentFilter filter = info.getIntentFilter();
        filter.setPriority(priority);
        if (isDefault) filter.addCategory(Intent.CATEGORY_DEFAULT);
        for (String action : actions) filter.addAction(action);
        return info;
    }

    private static ParsedActivityImpl activity(String name, ParsedIntentInfoImpl... filters) {
        ParsedActivityImpl activity = new ParsedActivityImpl();
        activity.setName(name);
        activity.setEnabled(true);
        activity.setExported(false); // Querying options does not imply permission to start them.
        for (ParsedIntentInfoImpl filter : filters) activity.addIntent(filter);
        return activity;
    }

    private static ApkPackage record(String name, boolean addon, boolean enabled,
            ParsedActivityImpl... activities) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setTargetSdkVersion(36);
        parsed.setEnabled(enabled);
        for (ParsedActivityImpl activity : activities) {
            activity.setPackageName(name);
            parsed.addActivity(activity);
        }
        parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(pkg, "packageName", name);
        field(pkg, "parsed", parsed);
        field(pkg, "apkPath", "/fixture/" + name + "/base.apk");
        field(pkg, "mUid", addon ? 1000 : 12001);
        field(pkg, "mDataDir", "/fixture/" + name + "/data");
        field(pkg, "mSystemAddon", addon);
        field(pkg, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return pkg;
    }

    private static PackageManagerService service(ApkPackage... records) throws Exception {
        PackageManagerService service = (PackageManagerService) unsafe.allocateInstance(PackageManagerService.class);
        Map<String, ApkPackage> packages = new LinkedHashMap<>();
        for (ApkPackage record : records) packages.put(record.packageName, record);
        field(service, "mPackages", packages);
        field(service, "mRemoving", new LinkedHashMap<String, ApkPackage>());
        return service;
    }

    @SuppressWarnings("unchecked")
    private static List<ResolveInfo> query(IPackageManager service, ComponentName caller,
            Intent[] specifics, String[] types, Intent generic, String type, long flags) throws Exception {
        ParceledListSlice<ResolveInfo> slice = service.queryIntentActivityOptions(
                caller, specifics, types, generic, type, flags, 0);
        check(slice != null, "queryIntentActivityOptions returned null instead of a result list");
        return slice.getList();
    }

    private static List<ResolveInfo> generic(IPackageManager service, long flags) throws Exception {
        return query(service, null, null, null, new Intent(ACTION), null, flags);
    }

    private static ComponentName component(String pkg, String activity) {
        return new ComponentName(pkg, activity);
    }

    private static Intent explicit(String pkg, String activity) {
        return new Intent().setComponent(component(pkg, activity));
    }

    private static void names(List<ResolveInfo> results, String... expected) {
        List<String> actual = new ArrayList<>();
        for (ResolveInfo result : results) actual.add(result.activityInfo.name);
        check(actual.equals(Arrays.asList(expected)), "Wrong option order/identity: " + actual
                + "; expected " + Arrays.toString(expected));
    }

    public static void frameworkContracts() throws Exception {
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        unsafe = (Unsafe) singleton.get(null);
        ordering();
        specifics();
        filtersAndSelectors();
        installedState();
        System.out.println("ActivityOptionsContractTest framework passed: " + checks + " checks");
    }

    private static void ordering() throws Exception {
        ParsedIntentInfoImpl high = filter("ordinary positive", 50, true, ACTION);
        PackageManagerService pm = service(
                record("fixture.z", false, true, activity("Z", filter("z", 0, true, ACTION))),
                record("fixture.low", false, true, activity("Low", filter("low", -8, true, ACTION))),
                record("fixture.nodefault", false, true, activity("NoDefault", filter("nd", 0, false, ACTION))),
                record("fixture.a", false, true, activity("A", high)),
                record("fixture.system", true, true, activity("System", filter("system", 0, true, ACTION))),
                record("fixture.priority", true, true, activity("Priority", filter("priority", 10, true, ACTION))));
        List<ResolveInfo> all = generic(pm, FILTER);
        names(all, "Priority", "System", "A", "Z", "NoDefault", "Low");
        check(all.get(2).priority == 0 && all.get(2).filter.getPriority() == 0
                && high.getIntentFilter().getPriority() == 50,
                "Non-privileged priority was not capped on a detached result");
        check(all.get(0).priority == 10 && all.get(0).system && all.get(1).system
                && !all.get(2).system, "Actual addon priority/system state was lost");
        check(all.get(2).specificIndex == -1 && all.get(2).preferredOrder == 0
                && all.get(2).userHandle.getIdentifier() == 0, "Generic resolver fields changed");
        check("ordinary positive".contentEquals(all.get(2).nonLocalizedLabel)
                && "ordinary positive".contentEquals(all.get(2).loadLabel(null))
                && all.get(2).labelRes == 71 && all.get(2).icon == 83
                && all.get(2).iconResourceId == 83, "Menu option filter label/icon was lost");
        names(generic(pm, PackageManager.MATCH_DEFAULT_ONLY), "Priority", "System", "A", "Z", "Low");
        for (ResolveInfo result : generic(pm, 0)) check(result.filter == null, "Unrequested filter leaked");
        names(generic(pm, PackageManager.MATCH_SYSTEM_ONLY), "Priority", "System");
        names(generic(pm, PackageManager.MATCH_VISIBLE_TO_INSTANT_APP_ONLY
                | PackageManager.MATCH_EXPLICITLY_VISIBLE_ONLY),
                "Priority", "System", "A", "Z", "NoDefault", "Low");
        check(pm.queryIntentActivityOptions(null, null, null, new Intent(ACTION), null, 0, 10)
                .getList().isEmpty(), "Unknown user received installed activities");
        PackageManagerService protectedPm = service(record("fixture.protected", true, true,
                activity("Protected", filter("protected", 100, true, ACTION, Intent.ACTION_VIEW))));
        check(generic(protectedPm, FILTER).get(0).priority == 0,
                "Protected action bypassed Android's privileged priority cap");
        all.get(0).filter.addAction("caller.modified");
        all.get(0).activityInfo.name = "CallerChanged";
        List<ResolveInfo> fresh = generic(pm, FILTER);
        check("Priority".equals(fresh.get(0).activityInfo.name)
                && !fresh.get(0).filter.hasAction("caller.modified"), "Result mutation changed installed filters/metadata");
    }

    private static void specifics() throws Exception {
        PackageManagerService pm = service(record("fixture.options", false, true,
                activity("A", filter("A", 0, true, ACTION, EXTRA)),
                activity("B", filter("B", 0, true, ACTION, EXTRA)),
                activity("C", filter("C", 0, true, ACTION)),
                activity("D", filter("D", 0, true, "fixture.options.OTHER"))));
        names(generic(pm, FILTER), "A", "C"); // Shared EXTRA collapses only the generic tail.
        List<ResolveInfo> sameAction = query(pm, null,
                new Intent[]{null, explicit("fixture.options", "B").setAction(ACTION),
                        new Intent("fixture.options.ABSENT"), explicit("fixture.options", "D")},
                null, new Intent(ACTION), null, FILTER);
        names(sameAction, "B", "D", "A", "C");
        check(sameAction.get(0).specificIndex == 1 && sameAction.get(1).specificIndex == 3
                && sameAction.get(2).specificIndex == -1, "Specific positions were renumbered");
        List<ResolveInfo> implicit = query(pm, null, new Intent[]{new Intent(EXTRA)}, null,
                new Intent(ACTION), null, FILTER);
        names(implicit, "A", "C");
        check(implicit.get(0).specificIndex == 0, "Implicit specific failed to resolve a real installed target");
        // Pinned Android explicit-specific behavior reuses the first action-duplicate generic
        // result, even when its activity differs from the requested component.
        List<ResolveInfo> reused = query(pm, null,
                new Intent[]{explicit("fixture.options", "B").setAction(EXTRA)}, null,
                new Intent(ACTION), null, FILTER);
        names(reused, "A", "C");
        check("A".contentEquals(reused.get(0).nonLocalizedLabel) && reused.get(0).specificIndex == 0,
                "Explicit-specific generic-entry reuse lost its associated metadata");
        Intent[] repeated = {explicit("fixture.options", "C"), null, explicit("fixture.options", "C")};
        List<ResolveInfo> duplicates = query(pm, null, repeated, null, new Intent(ACTION), null, 0);
        names(duplicates, "C", "C", "A");
        check(duplicates.get(0).specificIndex == 0 && duplicates.get(1).specificIndex == 2,
                "Distinct specific Intents for one component were collapsed");
        for (ResolveInfo result : duplicates) check(result.filter == null, "Specific filter escaped flag projection");
        names(query(pm, component("fixture.options", "C"), repeated, null, new Intent(ACTION), null, 0),
                "C", "A"); // Upstream excludes only the first caller occurrence.
        names(query(pm, component("fixture.options", "A"), null, null, new Intent(ACTION), null, 0), "C");
        // Explicit generic resolution has no filter; an unrelated specific action must not NPE.
        names(query(pm, null, new Intent[]{explicit("fixture.options", "D").setAction(EXTRA)}, null,
                explicit("fixture.options", "C"), null, 0), "D", "C");
        check("B".equals(sameAction.get(0).activityInfo.name)
                && "C".equals(repeated[0].getComponent().getClassName()) && repeated[1] == null,
                "A later query mutated earlier results or the supplied specific Intents");
    }

    private static void filtersAndSelectors() throws Exception {
        ParsedIntentInfoImpl wild = filter("wildcard first", 0, true, ACTION);
        wild.getIntentFilter().addDataType("text/*");
        ParsedIntentInfoImpl exact = filter("exact second", 0, true, ACTION);
        exact.getIntentFilter().addDataType("text/plain");
        ParsedIntentInfoImpl scheme = filter("scheme", 0, true, ACTION);
        scheme.getIntentFilter().addDataScheme("fixture");
        scheme.getIntentFilter().addDataAuthority("host", null);
        scheme.getIntentFilter().addCategory("fixture.category.MATCH");
        UriRelativeFilterGroup relative = new UriRelativeFilterGroup(UriRelativeFilterGroup.ACTION_ALLOW);
        relative.addUriRelativeFilter(new UriRelativeFilter(UriRelativeFilter.PATH,
                PatternMatcher.PATTERN_LITERAL, "/path"));
        scheme.getIntentFilter().addUriRelativeFilterGroup(relative);
        PackageManagerService pm = service(record("fixture.filters", false, true,
                activity("Typed", wild, exact), activity("Scheme", scheme),
                activity("Plain", filter("plain", 0, true, ACTION))));
        List<ResolveInfo> unrestricted = query(pm, null, null, null, new Intent(ACTION), "text/plain", FILTER);
        names(unrestricted, "Typed");
        check("exact second".contentEquals(unrestricted.get(0).nonLocalizedLabel),
                "Unrestricted MIME index order selected an earlier wildcard manifest filter");
        List<ResolveInfo> restricted = query(pm, null, null, null,
                new Intent(ACTION).setPackage("fixture.filters"), "text/plain", FILTER);
        check("wildcard first".contentEquals(restricted.get(0).nonLocalizedLabel),
                "Package-restricted matching ignored first eligible manifest filter");
        check("exact second".contentEquals(query(pm, null, null, null,
                new Intent(ACTION), "text/*", FILTER).get(0).nonLocalizedLabel),
                "Wildcard request did not visit concrete subtype cut first");
        check("wildcard first".contentEquals(query(pm, null, null, null,
                new Intent(ACTION), "*/*", FILTER).get(0).nonLocalizedLabel),
                "Universal request did not use typed-action registration order");
        names(query(pm, null, new Intent[]{null, new Intent(ACTION).setType("image/png")},
                new String[]{null, "text/plain"}, new Intent("fixture.options.NONE"), null, FILTER), "Typed");
        check(query(pm, null, null, null, new Intent(ACTION), "image/png", 0).isEmpty(), "MIME mismatch matched");
        names(query(pm, null, new Intent[]{explicit("fixture.filters", "Plain").setType("image/png")},
                new String[]{"image/png"}, new Intent("fixture.options.NONE"), null,
                PackageManager.MATCH_DEFAULT_ONLY), "Plain");
        Intent uri = new Intent(ACTION, Uri.parse("fixture://host/path")).addCategory("fixture.category.MATCH");
        List<ResolveInfo> uriMatches = query(pm, null, null, null, uri, null, FILTER);
        names(uriMatches, "Scheme");
        uriMatches.get(0).filter.getUriRelativeFilterGroup(0).addUriRelativeFilter(
                new UriRelativeFilter(UriRelativeFilter.QUERY, PatternMatcher.PATTERN_LITERAL, "caller=changed"));
        List<ResolveInfo> freshUri = query(pm, null, null, null, uri, null, FILTER);
        names(freshUri, "Scheme");
        check(freshUri.get(0).filter.getUriRelativeFilterGroup(0).getUriRelativeFilters().size() == 1,
                "Mutable nested filter data escaped the installed manifest boundary");
        check(query(pm, null, null, null, new Intent(uri).addCategory("fixture.category.ABSENT"), null, 0)
                .isEmpty(), "Unmatched category accepted");
        check(query(pm, null, null, null, new Intent(uri).setData(Uri.parse("fixture://other/path")), null, 0)
                .isEmpty(), "Unmatched authority accepted");
        Intent selected = new Intent("fixture.options.OUTER");
        selected.setSelector(new Intent(ACTION).setPackage("fixture.filters"));
        names(query(pm, null, null, null, selected, null, FILTER), "Plain");
        check("exact second".contentEquals(query(pm, null, null, null, selected,
                "text/plain", FILTER).get(0).nonLocalizedLabel),
                "Selector package switched unrestricted MIME cuts to manifest traversal");
        check("fixture.options.OUTER".equals(selected.getAction())
                && ACTION.equals(selected.getSelector().getAction()), "Query rewrote the caller's selector");
        selected.setComponent(component("fixture.filters", "Typed"));
        names(query(pm, null, null, null, selected, null, 0), "Typed");
        check(query(pm, null, null, null, new Intent(), null, 0).isEmpty(),
                "Unrestricted action-null/no-data query invented a resolver index cut");
    }

    private static void installedState() throws Exception {
        ParsedActivityImpl disabled = activity("Disabled", filter("disabled", 0, true, ACTION));
        disabled.setEnabled(false);
        ParsedActivityImpl aware = activity("Aware", filter("aware", 0, true, ACTION));
        aware.setDirectBootAware(true);
        ApkPackage enabled = record("fixture.state", false, true, disabled, aware,
                activity("Unaware", filter("unaware", 0, true, ACTION)));
        ApkPackage off = record("fixture.off", false, false,
                activity("AppDisabled", filter("off", 0, true, ACTION)));
        PackageManagerService pm = service(enabled, off);
        names(generic(pm, 0), "Aware", "Unaware");
        names(generic(pm, PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS), "Aware", "Unaware");
        List<ResolveInfo> all = generic(pm, PackageManager.MATCH_DISABLED_COMPONENTS);
        names(all, "AppDisabled", "Disabled", "Aware", "Unaware");
        check(!all.get(0).activityInfo.applicationInfo.enabled && !all.get(1).activityInfo.enabled,
                "Including disabled state rewrote the reported enabled bits");
        names(generic(pm, PackageManager.MATCH_DIRECT_BOOT_AWARE), "Aware");
        names(generic(pm, PackageManager.MATCH_DIRECT_BOOT_UNAWARE), "Unaware");
        names(query(pm, null, new Intent[]{explicit("fixture.state", "Disabled")}, null,
                new Intent("fixture.options.NONE"), null, PackageManager.MATCH_DISABLED_COMPONENTS), "Disabled");
        check(query(pm, null, new Intent[]{explicit("fixture.state", "Disabled")}, null,
                new Intent("fixture.options.NONE"), null, 0).isEmpty(), "Disabled explicit target resolved");
        Map<String, ApkPackage> removing = new LinkedHashMap<>();
        removing.put(enabled.packageName, enabled);
        field(pm, "mRemoving", removing);
        check(generic(pm, 0).isEmpty(), "Pending removal remained available");
        check(query(pm, null, new Intent[]{explicit("fixture.state", "Aware")}, null,
                new Intent("fixture.options.NONE"), null, 0).isEmpty(), "Explicit pending removal remained available");
        field(pm, "mRemoving", new LinkedHashMap<String, ApkPackage>());
        field(pm, "mPackages", new LinkedHashMap<String, ApkPackage>());
        check(generic(pm, PackageManager.MATCH_UNINSTALLED_PACKAGES).isEmpty(),
                "Uninstalled flag fabricated retained install state");
        Intent web = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.invalid/"));
        List<ResolveInfo> browser = query(pm, null, null, null, web, null, FILTER);
        names(browser, BrowserPackageSupport.ACTIVITY);
        check(browser.get(0).filter != null && browser.get(0).handleAllWebDataURI,
                "Existing host browser adapter lost its real dispatch filter");
        ApkPackage installedBrowser = record(BrowserPackageSupport.PACKAGE, false, true,
                activity("InstalledBrowser", filter("installed", 0, true, ACTION)));
        pm = service(installedBrowser);
        removing.clear(); removing.put(installedBrowser.packageName, installedBrowser);
        field(pm, "mRemoving", removing);
        check(query(pm, null, null, null, web, null, 0).isEmpty(),
                "Removing an installed browser resurrected the host adapter in the same query");
    }

    private static void parcelAndPublicManager(String fixture) throws Exception {
        File data = Files.createTempDirectory("gesher-activity-options-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        File apkFile = new File(data, "base.apk");
        Files.copy(new File(fixture).toPath(), apkFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
        ApkPackage apk = ApkPackage.parse(apkFile.getPath());
        PackageManagerService service = new PackageManagerService(Collections.singletonList(apk));
        Class<?> standInClass = Class.forName("com.gesher.server.StandIn");
        standInClass.getMethod("setStrict", boolean.class).invoke(null, true);
        Object standIn = standInClass.getMethod("of", Class.class, IInterface.class)
                .invoke(null, IPackageManager.class, service);
        IBinder target = (IBinder) standInClass.getMethod("asBinder").invoke(standIn);
        IPackageManager wire = IPackageManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, request, reply, flags); }
        });
        Field field = ActivityThread.class.getDeclaredField("sPackageManager");
        field.setAccessible(true); field.set(null, wire);
        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        Context context = constructor.newInstance().getSystemContext();
        PackageManager client = context.getPackageManager();
        String pkg = "com.gesher.components";
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
        Intent secondary = explicit(pkg, pkg + ".SecondActivity");
        Intent[] specifics = {null, secondary};
        List<ResolveInfo> array = client.queryIntentActivityOptions(null, specifics, launcher,
                PackageManager.GET_RESOLVED_FILTER);
        names(array, pkg + ".SecondActivity", pkg + ".MainActivity");
        check(array.get(0).specificIndex == 1 && array.get(1).specificIndex == -1
                && array.get(1).filter.hasCategory(Intent.CATEGORY_LAUNCHER),
                "Actual manifest/result metadata was lost in AIDL Parcel");
        names(client.queryIntentActivityOptions(null, Arrays.asList(specifics), launcher,
                PackageManager.ResolveInfoFlags.of(0)), pkg + ".SecondActivity", pkg + ".MainActivity");
        List<ResolveInfo> noFilter = client.queryIntentActivityOptions(null, specifics, launcher, 0);
        check(noFilter.get(0).filter == null && noFilter.get(1).filter == null,
                "Public manager returned unrequested resolved filters");
        names(client.queryIntentActivityOptions(component(pkg, pkg + ".MainActivity"), specifics, launcher, 0),
                pkg + ".SecondActivity");
        names(client.queryIntentActivityOptions(null, new Intent[]{new Intent(pkg + ".SINGLE_TOP")},
                launcher, PackageManager.MATCH_DEFAULT_ONLY), pkg + ".SingleTopActivity");
        check(wire.queryIntentActivityOptions(null, null, null, launcher, null, 0, 10).getList().isEmpty(),
                "AIDL query escaped supported user state");
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 1, "Pass the actual components.apk fixture");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        frameworkContracts();
        parcelAndPublicManager(args[0]);
        System.out.println("ActivityOptionsContractTest all passed: " + checks + " checks");
    }
}
