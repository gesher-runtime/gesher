/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.ParcelableException;
import android.os.RemoteException;
import android.os.UserHandle;

import com.android.internal.os.ApplicationSharedMemory;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.GmsSignatureSpoofing;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PackageUids;
import com.gesher.server.StandIn;

import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import sun.misc.Unsafe;

/** Real PMS/static manifest rules and public manager, with controlled caller identity. */
public final class PackageVisibilityContractTest {
    private static final String A = "fixture.visibility.a", B = "fixture.visibility.b";
    private static final String C = "fixture.visibility.c", ACTION = "fixture.visibility.ACTION";
    private static final Unsafe UNSAFE;
    private static int checks;
    private static boolean art;
    static {
        try { Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true); UNSAFE = (Unsafe)field.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Call { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Call call, String message) throws Exception {
        try { call.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(message + ": " + failure, failure);
            checks++; return;
        }
        throw new AssertionError(message + ": no exception");
    }
    private static Object get(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(owner);
    }
    private static void set(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); field.set(owner, value);
    }
    private static PackageImpl parsed(String name) {
        PackageImpl result = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        result.setTargetSdkVersion(36); result.setEnabled(true); return result;
    }
    private static ApkPackage record(PackageImpl parsed) throws Exception {
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", parsed.getPackageName()); set(result, "parsed", parsed);
        set(result, "mUid", PackageUids.uidFor(parsed.getPackageName()));
        return result;
    }
    private static ParsedIntentInfoImpl filter(String action) {
        ParsedIntentInfoImpl result = new ParsedIntentInfoImpl();
        result.getIntentFilter().addAction(action); return result;
    }
    private static ParsedActivityImpl activity(String name, boolean exported, ParsedIntentInfoImpl info) {
        ParsedActivityImpl result = new ParsedActivityImpl(); result.setName(name);
        result.setExported(exported); result.setEnabled(false); result.addIntent(info); return result;
    }
    private static ParsedProviderImpl provider(String authorities, boolean exported) {
        ParsedProviderImpl result = new ParsedProviderImpl(); result.setName("Provider");
        result.setAuthority(authorities); result.setExported(exported); return result;
    }
    private static void permission(PackageImpl parsed, String name) {
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(name, 0, Set.of()));
    }
    private static PackageManagerService service(ApkPackage... records) throws Exception {
        PackageManagerService result;
        if (art) {
            result = new PackageManagerService(Arrays.asList(records));
        } else {
            // The unrelated installer constructor creates native Binder objects.
            // Keep the actual package map, PermissionPolicy and query implementation.
            result = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            Map<String, ApkPackage> installed = new LinkedHashMap<>();
            for (ApkPackage record : records) installed.put(record.packageName, record);
            set(result, "mPackages", installed); set(result, "mRemoving", Map.of());
            var constructor = Class.forName("com.gesher.server.PermissionPolicy")
                    .getDeclaredConstructor(PackageManagerService.class);
            constructor.setAccessible(true); set(result, "mPermissions", constructor.newInstance(result));
        }
        if (!art) {
            // Host omits binary framework-resource JNI; supply only this native
            // catalog boundary. The Windows fixture reads the actual framework APK.
            Object catalog = get(result.permissions(), "mCatalog");
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            @SuppressWarnings("unchecked") Set<String> broadcasts = (Set<String>)get(catalog, "mProtectedBroadcasts");
            broadcasts.add(Intent.ACTION_BOOT_COMPLETED);
        }
        return result;
    }
    private static PackageManager client(PackageManagerService implementation) throws Exception {
        IPackageManager service = implementation;
        if (art) {
            StandIn.setStrict(true);
            IBinder target = StandIn.of(IPackageManager.class, implementation).asBinder();
            service = IPackageManager.Stub.asInterface(new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                        throws RemoteException { return target.transact(code, data, reply, flags); }
            });
        }
        // This method only uses ContextImpl.getUserId. The separate parsed-APK
        // PackageQueryContractTest constructs the complete real system context.
        Object context = UNSAFE.allocateInstance(Class.forName("android.app.ContextImpl"));
        set(context, "mUser", UserHandle.of(0));
        PackageManager manager = (PackageManager)UNSAFE.allocateInstance(Class.forName("android.app.ApplicationPackageManager"));
        set(manager, "mContext", context); set(manager, "mPM", service); return manager;
    }
    private static boolean query(PackageManagerService pm, String source, String target) throws Exception {
        return client(pm).canPackageQuery(source, target);
    }
    private static void as(int uid, Call call) throws Exception {
        long old = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | 981001L); call.run(); }
        finally { Binder.restoreCallingIdentity(old); }
    }
    private static void shapesAndCallers() throws Exception {
        ApkPackage a = record(parsed(A)), b = record(parsed(B));
        PackageManagerService pm = service(a, b); PackageManager client = client(pm);
        // Negative-control head throws here in the actual public wrapper: its
        // inherited Binder implementation returns null and the wrapper indexes [0].
        check(client.canPackageQuery(A, A), "Public self-package query failed");
        boolean shared = a.uid() == b.uid();
        check(shared != GmsSignatureSpoofing.isEnabled(), "Fixture did not use actual runtime UID mode");
        check(client.canPackageQuery(A, B) == shared, "Unrelated distinct UID became visible");
        check(Arrays.equals(client.canPackageQuery(A, new String[]{B, A, "android", B}),
                new boolean[]{shared, true, true, shared}), "Bulk query lost order, duplicate or false values");
        boolean[] first = client.canPackageQuery(A, new String[0]);
        check(first.length == 0 && first != client.canPackageQuery(A, new String[0]), "Empty result missing or reused");
        check(client.canPackageQuery("android", A), "Core package could not query installed state");
        expect(PackageManager.NameNotFoundException.class, () -> client.canPackageQuery("absent", A), "Missing source accepted");
        expect(PackageManager.NameNotFoundException.class, () -> client.canPackageQuery(A, "absent"), "Missing target accepted");
        expect(PackageManager.NameNotFoundException.class, () -> client.canPackageQuery("absent", new String[0]), "Empty array skipped source validation");
        expect(PackageManager.NameNotFoundException.class, () -> client.canPackageQuery(A, new String[]{A, null}), "Null array member lost typed missing-package failure");
        expect(NullPointerException.class, () -> client.canPackageQuery(null, A), "Null public source accepted");
        expect(NullPointerException.class, () -> client.canPackageQuery(A, (String)null), "Null public target accepted");
        expect(NullPointerException.class, () -> client.canPackageQuery(A, (String[])null), "Null public array accepted");
        expect(NullPointerException.class, () -> pm.canPackageQuery(A, null, 10), "Null wire array bypassed Android ordering");
        expect(ParcelableException.class, () -> pm.canPackageQuery(null, new String[0], 0), "Raw null source not rejected");
        check(Arrays.equals(pm.canPackageQuery(null, new String[]{null, "absent"}, 10), new boolean[2])
                && pm.canPackageQuery("absent", new String[0], -1).length == 0, "Absent user performed package lookups");
        as(110001, () -> expect(SecurityException.class, () -> pm.canPackageQuery(A, new String[]{A}, 0), "Cross-user query bypassed permission gate"));
        as(19999, () -> expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, A), "Unknown app UID gained visibility"));
        if (!shared) {
            as(a.uid(), () -> expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, B), "Caller-hidden target returned a boolean"));
            as(b.uid(), () -> expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, B), "Caller-hidden source returned a boolean"));
            as(b.uid(), () -> expect(PackageManager.NameNotFoundException.class, () -> client(pm).canPackageQuery(A, new String[0]), "Empty array skipped caller visibility"));
            PackageImpl observer = parsed(C); observer.addQueriesPackage(A); observer.addQueriesPackage(B);
            ApkPackage observerRecord = record(observer); PackageManagerService thirdParty = service(a, b, observerRecord);
            as(observerRecord.uid(), () -> check(!query(thirdParty, A, B), "A visible third-party source was rejected or granted visibility"));
        }
        set(pm, "mRemoving", Map.of(A, a));
        expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, A), "Removing source stayed queryable");
        set(pm, "mRemoving", Map.of(B, b));
        expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, B), "Removing target stayed queryable");
        set(pm, "mRemoving", Map.of()); set(pm, "mPackages", Map.of(A, a));
        expect(PackageManager.NameNotFoundException.class, () -> query(pm, A, B), "Removed package stayed in visibility snapshot");
        set(pm, "mPackages", Map.of(A, a, B, b));
        check(query(pm, A, B) == shared, "Reinstall did not publish fresh visibility");
    }
    private static void declarations() throws Exception {
        ApkPackage target = record(parsed(B));
        PackageImpl legacy = parsed(A); legacy.setTargetSdkVersion(29);
        check(query(service(record(legacy), target), A, B), "Legacy target lost visibility");
        legacy.setTargetSdkVersion(30);
        check(!query(service(record(legacy), target), A, B), "Android R target bypassed filtering");
        PackageImpl all = parsed(A); permission(all, Manifest.permission.QUERY_ALL_PACKAGES);
        check(query(service(record(all), target), A, B), "Declared QUERY_ALL_PACKAGES ignored");
        PackageImpl exact = parsed(A); exact.addQueriesPackage(B);
        PackageManagerService pm = service(record(exact), target, record(parsed(C)));
        check(Arrays.equals(client(pm).canPackageQuery(A, new String[]{C, B, B}), new boolean[]{false, true, true}), "Explicit queries packages changed meaning/order");
        set(pm, "mPackages", Map.of(A, record(parsed(A)), B, target));
        check(!query(pm, A, B), "Replacement kept stale manifest visibility");
        ApkPackage declaring = record(exact), sibling = record(parsed(C));
        set(sibling, "mUid", target.uid());
        PackageManagerService sharedTarget = service(declaring, target, sibling);
        check(query(sharedTarget, A, C), "Target UID sibling did not inherit the manifest visibility edge");
        set(sharedTarget, "mPackages", Map.of(A, declaring, C, sibling));
        check(!query(sharedTarget, A, C), "Removed target sibling kept its visibility edge");
        ApkPackage source = record(parsed(A));
        PackageImpl sharedQueries = parsed(C); sharedQueries.addQueriesPackage(B);
        ApkPackage sharedSource = record(sharedQueries); set(sharedSource, "mUid", source.uid());
        check(query(service(source, sharedSource, target), A, B), "Source UID sibling's declaration was ignored");
        PackageImpl force = parsed(B); force.setForceQueryable(true);
        ApkPackage forceRecord = record(force);
        check(!query(service(record(parsed(A)), forceRecord), A, B), "Ordinary APK self-granted force-queryable");
        set(forceRecord, "mSystemAddon", true);
        check(query(service(record(parsed(A)), forceRecord), A, B), "Actual system-addon force-queryable flag ignored");
        PackageImpl library = parsed(B); library.addLibraryName("fixture.library");
        PackageImpl user = parsed(A); user.addUsesLibrary("fixture.library");
        check(query(service(record(user), record(library)), A, B), "Required library provider hidden");
        user = parsed(A); user.addUsesOptionalLibrary("fixture.library");
        check(query(service(record(user), record(library)), A, B), "Optional library provider hidden");
        check(!query(service(record(user), record(library)), B, A), "Library visibility was made symmetric");
        PackageImpl definer = parsed(B); ParsedPermissionImpl permission = new ParsedPermissionImpl();
        permission.setName("fixture.visibility.PERMISSION"); definer.addPermission(permission);
        PackageImpl requester = parsed(A); permission(requester, permission.getName());
        check(query(service(record(requester), record(definer)), A, B), "Permission requester could not see definer");
        check(!query(service(record(requester), record(definer)), B, A), "Permission visibility was made symmetric");
        PackageImpl staticLibrary = parsed(B); staticLibrary.setStaticSharedLibrary(true);
        check(query(service(record(parsed(A)), record(staticLibrary)), A, B), "Static shared library was filtered as an application");
        if (art) {
            PackageImpl configured = parsed("com.android.settings"); ApkPackage configuredRecord = record(configured);
            check(!query(service(record(parsed(A)), configuredRecord), A, configured.getPackageName()), "Ordinary app acquired device force-queryable name");
            set(configuredRecord, "mSystemAddon", true);
            check(query(service(record(parsed(A)), configuredRecord), A, configured.getPackageName()), "Actual framework resource force-queryable policy ignored");
        }
    }
    private static void components() throws Exception {
        PackageImpl source = parsed(A); source.addQueriesIntent(new Intent(ACTION));
        PackageImpl target = parsed(B); target.addActivity(activity("DisabledActivity", true, filter(ACTION)));
        check(query(service(record(source), record(target)), A, B), "Exported disabled/non-default activity visibility lost");
        target = parsed(B); target.addActivity(activity("PrivateActivity", false, filter(ACTION)));
        check(!query(service(record(source), record(target)), A, B), "Private activity granted visibility");
        target = parsed(B); ParsedServiceImpl service = new ParsedServiceImpl(); service.setName("Service");
        service.setExported(true); service.addIntent(filter(ACTION)); target.addService(service);
        check(query(service(record(source), record(target)), A, B), "Exported service visibility lost");
        target = parsed(B); target.addReceiver(activity("Receiver", true, filter(ACTION)));
        check(query(service(record(source), record(target)), A, B), "Exported receiver visibility lost");
        target = parsed(B); target.addReceiver(activity("Receiver", true, filter(ACTION)));
        target.addProtectedBroadcast(ACTION);
        check(!query(service(record(source), record(target)), A, B), "Protected broadcast receiver granted visibility");
        source = parsed(A); source.addQueriesIntent(new Intent(Intent.ACTION_BOOT_COMPLETED));
        target = parsed(B); target.addReceiver(activity("BootReceiver", true, filter(Intent.ACTION_BOOT_COMPLETED)));
        check(!query(service(record(source), record(target)), A, B), "Framework protected broadcast granted receiver visibility");
        target = parsed(B); target.addActivity(activity("BootActivity", true, filter(Intent.ACTION_BOOT_COMPLETED)));
        check(query(service(record(source), record(target)), A, B), "Receiver-only protected action exclusion blocked an activity");
        source = parsed(A); source.addQueriesIntent(new Intent(ACTION));
        target = parsed(B); ParsedProviderImpl intentProvider = provider("fixture.authority", true);
        intentProvider.addIntent(filter(ACTION)); target.addProvider(intentProvider);
        check(query(service(record(source), record(target)), A, B), "Provider intent visibility lost");
        source = parsed(A); source.addQueriesProvider("fixture.second");
        target = parsed(B); target.addProvider(provider("fixture.first;fixture.second", true));
        check(query(service(record(source), record(target)), A, B), "Secondary provider authority ignored");
        target = parsed(B); target.addProvider(provider("fixture.second.extra", true));
        check(!query(service(record(source), record(target)), A, B), "Provider authority prefix granted visibility");
        target = parsed(B); target.addProvider(provider("fixture.second", false));
        check(!query(service(record(source), record(target)), A, B), "Private provider authority granted visibility");
        source = parsed(A); Intent query = new Intent("*").setDataAndType(Uri.parse("content://fixture/item"), "image/*");
        query.addCategory("fixture.category"); source.addQueriesIntent(query);
        ParsedIntentInfoImpl rich = filter(ACTION); IntentFilter f = rich.getIntentFilter();
        f.addDataScheme("content"); f.addDataType("image/png"); f.addCategory("fixture.category");
        target = parsed(B); target.addActivity(activity("MimeActivity", true, rich));
        check(query(service(record(source), record(target)), A, B), "Wildcard action/MIME query did not match real filter");
        query.setType("text/plain");
        check(!query(service(record(source), record(target)), A, B), "MIME mismatch granted visibility");
        query.setDataAndType(Uri.parse("file:///fixture/item"), "image/png");
        check(!query(service(record(source), record(target)), A, B), "Scheme mismatch granted visibility");
        query.setDataAndType(Uri.parse("content://fixture/item"), "image/png"); query.addCategory("fixture.absent");
        check(!query(service(record(source), record(target)), A, B), "Absent category granted visibility");
        check(f.hasAction(ACTION) && !f.hasAction("*") && f.countDataTypes() == 1,
                "Visibility matching mutated installed filters");
    }
    public static int run(boolean onArt) throws Exception {
        art = onArt; checks = 0; shapesAndCallers();
        if (GmsSignatureSpoofing.isEnabled()) { declarations(); components(); }
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("package-visibility-").toString(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        System.out.println("PackageVisibilityContractTest all passed: " + run(true) + " checks");
    }
}
