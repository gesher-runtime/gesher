/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.search;

import android.Manifest;
import android.app.ISearchManager;
import android.app.SearchManager;
import android.app.SearchableInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Bundle;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Function;
import sun.misc.Unsafe;

/** Production lookup, public client and visibility; explicit native identity/XML boundaries. */
public final class SearchableHost {
    private static final String A = "fixture.search.client", B = "fixture.search.target";
    private static final int UID_A = 10000, UID_B = 10001, PID = 981017;
    private static final String META = "android.app.searchable", DEFAULT = "android.app.default_searchable";
    private static Unsafe unsafe;
    private static int checks;
    private interface Checked { void run() throws Exception; }

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static void set(Object value, String name, Object contents) throws Exception {
        field(value.getClass(), name).set(value, contents);
    }
    private static PackageImpl manifest(String name) {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setTargetSdkVersion(36); parsed.setEnabled(true); return parsed;
    }
    private static ParsedActivityImpl activity(String pkg, String name, boolean searchable) {
        ParsedActivityImpl result = new ParsedActivityImpl();
        result.setPackageName(pkg); result.setName(pkg + "." + name);
        result.setEnabled(true); result.setExported(false);
        if (searchable) {
            ParsedIntentInfoImpl filter = new ParsedIntentInfoImpl();
            filter.getIntentFilter().addAction(Intent.ACTION_SEARCH); result.addIntent(filter);
            Bundle metadata = new Bundle(); metadata.putInt(META, 0x7f020001); result.setMetaData(metadata);
        }
        return result;
    }
    private static void redirect(ParsedActivityImpl activity, String target) {
        Bundle metadata = new Bundle(); metadata.putString(DEFAULT, target); activity.setMetaData(metadata);
    }
    private static PackageImpl target() {
        PackageImpl result = manifest(B);
        result.addActivity(activity(B, "Search", true));
        ParsedActivityImpl launch = activity(B, "Launch", false); redirect(launch, ".Search"); result.addActivity(launch);
        ParsedActivityImpl chain = activity(B, "Chain", false); redirect(chain, ".Launch"); result.addActivity(chain);
        ParsedActivityImpl star = activity(B, "Star", false); redirect(star, "*"); result.addActivity(star);
        ParsedActivityImpl empty = activity(B, "Empty", false); redirect(empty, ""); result.addActivity(empty);
        ParsedActivityImpl direct = activity(B, "Direct", true);
        Bundle both = new Bundle(direct.getMetaData()); both.putString(DEFAULT, ".Search"); direct.setMetaData(both);
        result.addActivity(direct);
        result.addActivity(activity(B, "AppDefault", false));
        Bundle application = new Bundle(); application.putString(DEFAULT, B + ".Search"); result.setMetaData(application);
        return result;
    }
    private static ApkPackage record(PackageImpl parsed, int uid) throws Exception {
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(result, "packageName", parsed.getPackageName()); set(result, "parsed", parsed);
        set(result, "apkPath", "/fixture/" + parsed.getPackageName() + "/base.apk");
        set(result, "mDataDir", "/fixture/" + parsed.getPackageName() + "/data");
        set(result, "mUid", uid); set(result, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return result;
    }
    private static SearchableInfo metadata(ActivityInfo activity) {
        try {
            SearchableInfo result = (SearchableInfo)unsafe.allocateInstance(SearchableInfo.class);
            set(result, "mSearchActivity", new ComponentName(activity.packageName, activity.name));
            set(result, "mLabelId", 0x7f010001); return result;
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static final class Fixture {
        final ApkPackage caller, target;
        final PackageManagerService packages;
        final Map<String, ApkPackage> installed = new LinkedHashMap<>();
        final ISearchManager service;
        final SearchManager client;
        int reads;
        Function<ActivityInfo, SearchableInfo> reader = SearchableHost::metadata;
        Fixture(PackageImpl source, PackageImpl destination, int targetUid) throws Exception {
            caller = record(source, UID_A); target = record(destination, targetUid);
            installed.put(A, caller); installed.put(B, target);
            packages = (PackageManagerService)unsafe.allocateInstance(PackageManagerService.class);
            set(packages, "mPackages", installed); set(packages, "mRemoving", Map.of());
            var permissionPolicy = Class.forName("com.gesher.server.PermissionPolicy")
                    .getDeclaredConstructor(PackageManagerService.class);
            permissionPolicy.setAccessible(true);
            set(packages, "mPermissions", permissionPolicy.newInstance(packages));
            ActivityTaskManagerService activities = (ActivityTaskManagerService)unsafe.allocateInstance(ActivityTaskManagerService.class);
            Class<?> processClass = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
            Object process = unsafe.allocateInstance(processClass);
            set(process, "pid", PID); set(process, "uid", UID_A); set(process, "packageName", A);
            set(process, "packageCode", caller); set(activities, "mProcesses", new ArrayList<>(List.of(process)));
            CallerIdentity callers = new CallerIdentity(packages, activities);
            Class<?> type = Class.forName("com.gesher.server.SearchManagerService");
            var constructor = type.getDeclaredConstructor(PackageManagerService.class, CallerIdentity.class, Function.class);
            constructor.setAccessible(true);
            service = (ISearchManager)constructor.newInstance(packages, callers,
                    (Function<ActivityInfo, SearchableInfo>)activity -> {
                        check(Binder.getCallingUid() == 1000, "XML loading retained the untrusted Binder identity");
                        reads++; return reader.apply(activity);
                    });
            client = (SearchManager)unsafe.allocateInstance(SearchManager.class);
            set(client, "mService", service);
        }
        SearchableInfo lookup(String name) { return client.getSearchableInfo(new ComponentName(B, B + "." + name)); }
        void enabled(String name, int state) throws Exception {
            var method = ApkPackage.class.getDeclaredMethod("setComponentEnabledState", String.class, int.class);
            method.setAccessible(true); method.invoke(target, B + "." + name, state);
        }
    }
    private static PackageImpl visibleClient() { PackageImpl source = manifest(A); source.addQueriesPackage(B); return source; }
    private static void as(int uid, int pid, Checked call) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL)); call.run(); }
        finally { Binder.restoreCallingIdentity(previous); }
    }
    private static void visibility() throws Exception {
        Fixture explicit = new Fixture(visibleClient(), target(), UID_B);
        as(UID_A, PID, () -> {
            check(explicit.lookup("Search") != null, "Explicit queries package was hidden by package equality");
            check(Binder.getCallingUid() == UID_A && Binder.getCallingPid() == PID, "Caller identity was not restored");
        });
        Fixture hidden = new Fixture(manifest(A), target(), UID_B);
        as(UID_A, PID, () -> check(hidden.lookup("Search") == null, "Unrelated invisible package metadata leaked"));
        check(hidden.reads == 0, "Invisible APK was read before visibility denial");
        PackageImpl all = manifest(A); all.addUsesPermission(new ParsedUsesPermissionImpl(Manifest.permission.QUERY_ALL_PACKAGES, 0, Set.of()));
        Fixture queryAll = new Fixture(all, target(), UID_B);
        as(UID_A, PID, () -> check(queryAll.lookup("Search") != null, "QUERY_ALL_PACKAGES was ignored"));
        PackageImpl old = manifest(A); old.setTargetSdkVersion(29);
        Fixture legacy = new Fixture(old, target(), UID_B);
        as(UID_A, PID, () -> check(legacy.lookup("Search") != null, "Pre-R package visibility changed"));
        Fixture shared = new Fixture(manifest(A), target(), UID_A);
        as(UID_A, PID, () -> check(shared.lookup("Search") != null, "Shared UID package metadata was hidden"));
        as(1000, PID + 1, () -> check(hidden.lookup("Search") != null, "System lookup was incorrectly app-local"));
        as(UID_A, PID + 1, () -> {
            try { explicit.lookup("Search"); throw new AssertionError("Unknown process was accepted"); }
            catch (SecurityException expected) { checks++; }
        });
        Fixture otherUser = new Fixture(visibleClient(), target(), UID_B);
        as(110000, PID, () -> {
            try { check(otherUser.lookup("Search") == null, "Lookup crossed into a different user"); }
            catch (SecurityException expected) { checks++; }
        });
    }

    private static void legacyVisibility() throws Exception {
        PackageImpl client = manifest(A);
        client.addActivity(activity(A, "Search", true));
        Fixture hidden = new Fixture(client, target(), UID_A);
        as(UID_A, PID, () -> {
            SearchableInfo own = hidden.client.getSearchableInfo(new ComponentName(A, A + ".Search"));
            check(own != null, "Legacy caller could not resolve its own declaration");
            check(hidden.lookup("Search") == null, "Legacy compatibility UID exposed a hidden package");
        });
        check(hidden.reads == 1, "Legacy lookup read an invisible APK");

        Fixture visible = new Fixture(visibleClient(), target(), UID_A);
        as(UID_A, PID, () -> {
            check(visible.lookup("Search") != null, "Legacy visibility ignored the caller's queries");
            check(visible.lookup("Launch") != null, "Legacy visibility blocked a visible redirect");
        });

        PackageImpl old = manifest(A);
        old.setTargetSdkVersion(29);
        Fixture preR = new Fixture(old, target(), UID_A);
        as(UID_A, PID, () -> check(preR.lookup("Search") != null, "Pre-R legacy caller lost visibility"));
    }

    private static void routing() throws Exception {
        Fixture f = new Fixture(visibleClient(), target(), UID_B);
        as(UID_A, PID, () -> {
            check(f.service.getSearchableInfo(null) == null, "Raw null component was resolved");
            check(f.lookup("Missing") == null, "Missing source resolved the app default");
            check(f.lookup("Launch").getSearchActivity().getClassName().equals(B + ".Search"), "Activity default was not followed");
            check(f.lookup("AppDefault").getSearchActivity().getClassName().equals(B + ".Search"), "Application default was not followed");
            check(f.lookup("Direct").getSearchActivity().getClassName().equals(B + ".Direct"), "Default displaced direct metadata");
            check(f.lookup("Chain") == null, "Unsupported recursive default chain was followed");
            check(f.lookup("Star") == null && f.lookup("Empty") == null, "Unavailable default declaration fabricated a target");
            int before = f.reads;
            f.enabled("Search", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
            check(f.lookup("Launch") == null, "Disabled target metadata survived");
            check(f.reads == before, "Disabled target resource was read");
            f.enabled("Search", PackageManager.COMPONENT_ENABLED_STATE_ENABLED);
            f.enabled("Launch", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
            check(f.lookup("Launch") == null, "Disabled source redirected");
            check(f.lookup("Search") != null, "Unrelated disabled source hid a valid direct target");
        });
        PackageImpl noAction = manifest(B);
        ParsedActivityImpl declaration = activity(B, "Search", false);
        Bundle data = new Bundle(); data.putInt(META, 42); declaration.setMetaData(data); noAction.addActivity(declaration);
        Fixture missingFilter = new Fixture(visibleClient(), noAction, UID_B);
        as(UID_A, PID, () -> check(missingFilter.lookup("Search") == null, "Metadata without SEARCH/WEB_SEARCH filter was accepted"));
        check(missingFilter.reads == 0, "Nonsearchable resource was parsed");
        PackageImpl web = manifest(B); ParsedActivityImpl webActivity = activity(B, "Search", false);
        ParsedIntentInfoImpl filter = new ParsedIntentInfoImpl(); filter.getIntentFilter().addAction(Intent.ACTION_WEB_SEARCH);
        webActivity.addIntent(filter); webActivity.setMetaData(data); web.addActivity(webActivity);
        Fixture webSearch = new Fixture(visibleClient(), web, UID_B);
        as(UID_A, PID, () -> check(webSearch.lookup("Search") != null, "WEB_SEARCH declaration was ignored"));
    }
    private static void resourceAndLifecycle() throws Exception {
        Fixture f = new Fixture(visibleClient(), target(), UID_B);
        as(UID_A, PID, () -> {
            f.reader = info -> null;
            check(f.lookup("Search") == null, "Invalid framework XML fabricated a result");
            f.reader = info -> { throw new Resources.NotFoundException("fixture missing resource"); };
            check(f.lookup("Search") == null, "Missing resource did not return unavailable");
            check(Binder.getCallingUid() == UID_A, "Resource failure leaked cleared caller identity");
            f.reader = info -> { throw new IllegalStateException("fixture unexpected resource failure"); };
            try { f.lookup("Search"); throw new AssertionError("Unexpected resource error was hidden"); }
            catch (IllegalStateException expected) { checks++; }
            check(Binder.getCallingUid() == UID_A, "Unexpected exception leaked cleared identity");
            f.reader = info -> {
                try { f.enabled("Search", PackageManager.COMPONENT_ENABLED_STATE_DISABLED); }
                catch (Exception error) { throw new AssertionError(error); }
                return metadata(info);
            };
            check(f.lookup("Search") == null, "Disable during resource read published stale metadata");
            f.enabled("Search", PackageManager.COMPONENT_ENABLED_STATE_ENABLED);
            f.reader = info -> { f.installed.remove(B); return metadata(info); };
            check(f.lookup("Search") == null, "Uninstall during resource read published stale metadata");
        });
        Fixture replaced = new Fixture(visibleClient(), target(), UID_B);
        ApkPackage newTarget = record(target(), UID_B);
        replaced.reader = info -> { replaced.installed.put(B, newTarget); return metadata(info); };
        as(UID_A, PID, () -> check(replaced.lookup("Search") == null, "Replacement published old APK metadata"));
        Fixture revoked = new Fixture(visibleClient(), target(), UID_B);
        ApkPackage noQueries = record(manifest(A), UID_A);
        revoked.reader = info -> { revoked.installed.put(A, noQueries); return metadata(info); };
        as(UID_A, PID, () -> check(revoked.lookup("Search") == null, "Visibility revoked during parsing still published metadata"));
        Fixture removing = new Fixture(visibleClient(), target(), UID_B);
        set(removing.packages, "mRemoving", Map.of(B, removing.target));
        as(UID_A, PID, () -> check(removing.lookup("Search") == null, "Removal-pending package was readable"));
        check(removing.reads == 0, "Removal-pending package XML was read");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        if (args.length == 2 && "legacy".equals(args[1])) {
            legacyVisibility();
        } else {
            visibility(); routing(); resourceAndLifecycle();
        }
        System.out.println("Searchable host all passed: " + checks + " checks");
        System.out.println("Production SearchManagerService, PMS visibility and public SearchManager; constructed manifests, controlled Binder JNI and metadata-reader boundary. Real XML, ContentResolver and Binder transport remain the components/peer Windows APK gates.");
    }
}
