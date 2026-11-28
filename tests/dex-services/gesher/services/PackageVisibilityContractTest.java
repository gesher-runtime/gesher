/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.IPackageManager;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.res.AssetManager;
import android.content.res.Resources;
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
import com.gesher.server.PackageManagerService;
import com.gesher.server.PackageUids;
import com.gesher.server.StandIn;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
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
        return record(parsed, PackageUids.uidFor(parsed.getPackageName()));
    }
    private static ApkPackage record(PackageImpl parsed, int uid) throws Exception {
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", parsed.getPackageName()); set(result, "parsed", parsed);
        set(result, "mUid", uid);
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
        check(!shared, "Unrelated packages shared an identity in the current runtime UID mode");
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
        // AppsFilterImpl uses declared protected broadcasts only, not PackageManagerService's prefixes.
        String prefixed = "android.net.netmon.lingerExpired.fixture";
        source = parsed(A); source.addQueriesIntent(new Intent(prefixed));
        target = parsed(B); target.addReceiver(activity("PrefixReceiver", true, filter(prefixed)));
        check(query(service(record(source), record(target)), A, B), "Protected prefix removed receiver visibility");
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
    private static void installSourceVisibility() throws Exception {
        ApkPackage target = record(parsed(A)), installer = record(parsed(B));
        PackageImpl observer = parsed(C);
        observer.addQueriesPackage(A);
        ApkPackage observerRecord = record(observer);
        PackageManagerService pm = service(target, installer, observerRecord);
        String generation = "951630b6-0775-4a4c-9366-7d4a7cc2c433";
        // Bind the controlled test record to a specific installation, as Session does.
        set(installer, "mEnabledOwner", generation);
        Class<?> type = Class.forName("com.gesher.server.PackageInstallSource");
        var decode = type.getDeclaredMethod("decode", String.class);
        decode.setAccessible(true);
        Object attribution = decode.invoke(null, B + "," + generation + ",2");
        set(target, "mInstallSource", attribution);
        PackageManager publicClient = client(pm);
        as(target.uid(), () -> {
            InstallSourceInfo source = publicClient.getInstallSourceInfo(A);
            check(B.equals(source.getInstallingPackageName()) && B.equals(source.getInitiatingPackageName())
                    && source.getPackageSource() == PackageInstaller.PACKAGE_SOURCE_STORE,
                    "Own authenticated installer was filtered");
            check(B.equals(publicClient.getInstallerPackageName(A)) && publicClient.canPackageQuery(A, B),
                    "Legacy getter or installer visibility edge disagreed with install source");
        });
        if (target.uid() != observerRecord.uid()) {
            as(observerRecord.uid(), () -> check(publicClient.getInstallSourceInfo(A).getInstallingPackageName() == null,
                    "Observer learned about an otherwise hidden installer"));
            as(installer.uid(), () -> expect(PackageManager.NameNotFoundException.class,
                    () -> publicClient.getInstallSourceInfo(A), "Installer visibility was made bidirectional"));
        }
        // AOSP grants separate visibility edges to the still-installed initiator and
        // the reassigned installer. Third-party callers filter those two roles independently.
        String d = "fixture.visibility.d", e = "fixture.visibility.e";
        ApkPackage nextInstaller = record(parsed(d));
        set(nextInstaller, "mEnabledOwner", "932c12b3-57c3-40eb-9d6e-d0913c506914");
        PackageImpl seesNext = parsed(C); seesNext.addQueriesPackage(A); seesNext.addQueriesPackage(d);
        observerRecord = record(seesNext);
        PackageImpl seesInitiator = parsed(e); seesInitiator.addQueriesPackage(A); seesInitiator.addQueriesPackage(B);
        ApkPackage initiatingObserver = record(seesInitiator);
        var replace = type.getDeclaredMethod("withInstaller", ApkPackage.class); replace.setAccessible(true);
        set(target, "mInstallSource", replace.invoke(attribution, nextInstaller));
        set(pm, "mPackages", Map.of(A, target, B, installer, C, observerRecord, d, nextInstaller, e, initiatingObserver));
        as(target.uid(), () -> {
            InstallSourceInfo source = publicClient.getInstallSourceInfo(A);
            check(B.equals(source.getInitiatingPackageName()) && d.equals(source.getInstallingPackageName()),
                    "Reassignment replaced the historical initiator");
            check(publicClient.canPackageQuery(A, B) && publicClient.canPackageQuery(A, d),
                    "Installed initiator lost its visibility edge after reassignment");
        });
        if (target.uid() != observerRecord.uid()) {
            as(observerRecord.uid(), () -> {
                InstallSourceInfo source = publicClient.getInstallSourceInfo(A);
                check(source.getInitiatingPackageName() == null && d.equals(source.getInstallingPackageName()),
                        "Current installer visibility leaked the independent initiator");
            });
            as(initiatingObserver.uid(), () -> {
                InstallSourceInfo source = publicClient.getInstallSourceInfo(A);
                check(B.equals(source.getInitiatingPackageName()) && source.getInstallingPackageName() == null,
                        "Initiator visibility depended on visibility of the current installer");
            });
        }
        set(pm, "mPackages", Map.of(A, target, C, observerRecord));
        check(publicClient.getInstallSourceInfo(A).getInstallingPackageName() == null,
                "Removed installer remained installer of record");
        ApkPackage replacement = record(parsed(B));
        set(replacement, "mEnabledOwner", "5cebb932-01c0-4015-b0fa-b8218e4df7cd");
        set(pm, "mPackages", Map.of(A, target, B, replacement, C, observerRecord));
        check(publicClient.getInstallSourceInfo(A).getInstallingPackageName() == null,
                "A same-name reinstall inherited attribution from an earlier generation");

        var fromSession = type.getDeclaredMethod("fromSession", ApkPackage.class, int.class);
        fromSession.setAccessible(true);
        var encode = type.getDeclaredMethod("encode");
        encode.setAccessible(true);
        Object categoryOnly = fromSession.invoke(null, null, PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE);
        set(target, "mInstallSource", decode.invoke(null, encode.invoke(categoryOnly)));
        InstallSourceInfo categorySource = publicClient.getInstallSourceInfo(A);
        check(categorySource.getPackageSource() == PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE
                && categorySource.getInitiatingPackageName() == null
                && categorySource.getInstallingPackageName() == null,
                "Durable source category required or manufactured an installer identity");
    }

    private static boolean serviceQuery(PackageManagerService pm, String source, String target)
            throws Exception {
        var query = PackageManagerService.class.getDeclaredMethod(
                "canSeeInstalledPackage", String.class, String.class);
        query.setAccessible(true);
        return (Boolean) query.invoke(pm, source, target);
    }
    private static void authenticatedServiceVisibility() throws Exception {
        ApkPackage a = record(parsed(A)), b = record(parsed(B));
        PackageManagerService hidden = service(a, b);
        check(serviceQuery(hidden, A, A), "Authenticated service lookup hid its own package");
        check(!serviceQuery(hidden, A, B), "Authenticated service lookup exposed an unrelated package");
        check(!serviceQuery(hidden, "absent", B), "Unknown service caller gained visibility");
        PackageImpl queries = parsed(A); queries.addQueriesPackage(B);
        PackageManagerService visible = service(record(queries), b);
        check(serviceQuery(visible, A, B), "Authenticated service lookup ignored manifest queries");
        set(visible, "mRemoving", Map.of(B, b));
        check(!serviceQuery(visible, A, B), "Removing provider remained visible to service lookup");
        PackageImpl siblingQueries = parsed(C); siblingQueries.addQueriesPackage(B);
        ApkPackage sibling = record(siblingQueries); set(sibling, "mUid", a.uid());
        PackageManagerService shared = service(a, sibling, b);
        check(serviceQuery(shared, A, C), "Authenticated shared UID lost sibling visibility");
        check(serviceQuery(shared, A, B), "Authenticated shared UID lost sibling provider queries");
        set(shared, "mPackages", Map.of(A, a, B, b));
        check(!serviceQuery(shared, A, B), "Removed shared UID sibling kept its provider queries");
        PackageImpl legacy = parsed(A); legacy.setTargetSdkVersion(29);
        check(serviceQuery(service(record(legacy), b), A, B), "Legacy target lost provider visibility");
        PackageImpl all = parsed(A); permission(all, Manifest.permission.QUERY_ALL_PACKAGES);
        check(serviceQuery(service(record(all), b), A, B), "QUERY_ALL_PACKAGES lost provider visibility");
    }

    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method result = type.getDeclaredMethod(name, parameters); result.setAccessible(true); return result;
    }
    private static Object invoke(Method method, Object owner, Object... arguments) throws Exception {
        try { return method.invoke(owner, arguments); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception exception) throw exception;
            if (failure.getCause() instanceof Error error) throw error;
            throw failure;
        }
    }
    private static void publish(PackageManagerService pm, Map<String, ApkPackage> packages) throws Exception {
        synchronized (pm) { invoke(method(PackageManagerService.class, "publishPackages", Map.class), pm, packages); }
    }

    private static void snapshotLifetimes() throws Exception {
        ApkPackage source = record(parsed(A)), target = record(parsed(B));
        PackageImpl queries = parsed(C); queries.addQueriesPackage(B);
        ApkPackage sibling = record(queries, source.uid());
        PackageManagerService pm = service(source, target, sibling);
        PackageManager manager = client(pm);
        check(manager.canPackageQuery(A, B) && serviceQuery(pm, A, B), "Shared source edge missing");
        Object snapshot = get(pm, "mVisibilitySnapshot");
        check(get(snapshot, "publicPackages") != get(snapshot, "installedPackages"),
                "Public and installed-only definition sets were conflated");
        for (int i = 0; i < 20; i++) {
            check(manager.canPackageQuery(A, B) && serviceQuery(pm, A, B), "Snapshot changed a repeated result");
            check(get(pm, "mVisibilitySnapshot") == snapshot, "Unchanged package state rebuilt visibility");
        }
        Method begin = method(PackageManagerService.class, "beginPackageRemoval", ApkPackage.class);
        Method end = method(PackageManagerService.class, "endPackageRemoval", ApkPackage.class);
        invoke(begin, pm, sibling);
        check(get(pm, "mVisibilitySnapshot") == null, "Removal retained a metadata snapshot before another query");
        check(!manager.canPackageQuery(A, B) && !serviceQuery(pm, A, B), "Pending shared-UID removal retained its edge");
        invoke(end, pm, sibling);
        check(get(pm, "mVisibilitySnapshot") == null, "Removal rollback retained the removal snapshot");
        check(manager.canPackageQuery(A, B) && serviceQuery(pm, A, B), "Removal rollback failed to restore the edge");
        invoke(begin, pm, sibling);
        check(!manager.canPackageQuery(A, B), "Second pending removal retained its edge");
        invoke(method(PackageManagerService.class, "recoveredPackageRemovals"), pm);
        check(get(pm, "mVisibilitySnapshot") == null, "Recovery retained pending-removal metadata");
        check(manager.canPackageQuery(A, B) && serviceQuery(pm, A, B), "Recovery failed to restore the edge");
        ApkPackage replacement = record(parsed(C), source.uid());
        publish(pm, Map.of(A, source, B, target, C, replacement));
        check(get(pm, "mVisibilitySnapshot") == null, "Publication retained retired code before another query");
        check(!manager.canPackageQuery(A, B) && !serviceQuery(pm, A, B), "Replacement kept old manifest edges");
        publish(pm, Map.of(A, source, B, target, C, record(queries, target.uid())));
        check(!manager.canPackageQuery(A, B), "Old source app-ID membership survived a new generation");

        // Installer attribution changes independently of package publication.
        String generation = "3a5fd731-5016-4a62-b846-501288778d86";
        set(target, "mEnabledOwner", generation);
        Class<?> attribution = Class.forName("com.gesher.server.PackageInstallSource");
        Object installedBy = invoke(method(attribution, "decode", String.class), null, B + "," + generation + ",2");
        snapshot = get(pm, "mVisibilitySnapshot");
        Method installSource = method(ApkPackage.class, "setInstallSource", attribution);
        invoke(installSource, source, installedBy);
        check(manager.canPackageQuery(A, B) && serviceQuery(pm, A, B), "Cached metadata hid fresh installer attribution");
        check(get(pm, "mVisibilitySnapshot") == snapshot, "Attribution test unexpectedly rebuilt metadata");
        invoke(installSource, source, (Object)null);
        check(!manager.canPackageQuery(A, B) && !serviceQuery(pm, A, B), "Cleared installer attribution stayed visible");

        PackageImpl hostQueries = parsed(A); hostQueries.addQueriesPackage("com.gesher.browser");
        PackageManagerService hosts = service(record(hostQueries));
        check(query(hosts, A, "com.gesher.browser") && !serviceQuery(hosts, A, "com.gesher.browser"),
                "Installed-only lookup acquired a public host definition");
        hosts = service(record(hostQueries));
        check(!serviceQuery(hosts, A, "com.gesher.browser") && query(hosts, A, "com.gesher.browser"),
                "Installed-only cache hid a later public host definition");

        PackageImpl receiver = parsed(B); receiver.addReceiver(activity("Receiver", true, filter(ACTION)));
        PackageImpl observing = parsed(A); observing.addQueriesIntent(new Intent(ACTION));
        ApkPackage observer = record(observing), unprotected = record(receiver);
        PackageManagerService broadcasts = service(observer, unprotected);
        check(query(broadcasts, A, B), "Receiver edge missing before protected-broadcast publication");
        PackageImpl protectedReceiver = parsed(B);
        protectedReceiver.addReceiver(activity("Receiver", true, filter(ACTION)));
        protectedReceiver.addProtectedBroadcast(ACTION);
        publish(broadcasts, Map.of(A, observer, B, record(protectedReceiver, unprotected.uid())));
        check(!query(broadcasts, A, B), "Publication retained stale protected-broadcast metadata");
        publish(broadcasts, Map.of(A, observer, B, unprotected));
        check(query(broadcasts, A, B), "Retired protected broadcast remained in the snapshot");
    }

    /** Only the resource catalog boundary is controlled; the actual force-queryable rule runs. */
    private static final class VisibilityResources extends Resources {
        boolean all;
        String[] names;
        VisibilityResources() { super(null, null, null); }
        @Override public boolean getBoolean(int id) {
            if (id != com.android.internal.R.bool.config_forceSystemPackagesQueryable) throw new AssertionError(id);
            return all;
        }
        @Override public String[] getStringArray(int id) {
            if (id != com.android.internal.R.array.config_forceQueryablePackages) throw new AssertionError(id);
            return names;
        }
    }
    private static void liveResourcePolicy() throws Exception {
        ApkPackage source = record(parsed(A)), target = record(parsed(B));
        set(target, "mSystemAddon", true);
        PackageManagerService pm = service(source, target);
        Field system = Resources.class.getDeclaredField("mSystem"); system.setAccessible(true);
        Object original = system.get(null);
        VisibilityResources resources = (VisibilityResources)UNSAFE.allocateInstance(VisibilityResources.class);
        resources.names = new String[0];
        try {
            system.set(null, resources);
            check(!query(pm, A, B), "Unconfigured system addon became queryable");
            Object snapshot = get(pm, "mVisibilitySnapshot");
            resources.all = true;
            check(query(pm, A, B), "Live force-all resource was cached as a denial");
            resources.all = false; resources.names = new String[]{B};
            check(query(pm, A, B), "Live named force-queryable resource was ignored");
            resources.names = new String[0];
            check(!query(pm, A, B), "Retired resource grant was cached");
            check(get(pm, "mVisibilitySnapshot") == snapshot, "Resource test unexpectedly rebuilt metadata");
        } finally { system.set(null, original); }
    }

    private static void concurrentPublication() throws Exception {
        PackageImpl first = parsed(A); first.addQueriesPackage(B);
        PackageImpl second = parsed(A); second.addQueriesPackage(C);
        ApkPackage a = record(first), b = record(parsed(B)), c = record(parsed(C));
        ApkPackage replacement = record(second, a.uid());
        PackageManagerService pm = service(a, b, c);
        Map<String, ApkPackage> before = Map.of(A, a, B, b, C, c);
        Map<String, ApkPackage> after = Map.of(A, replacement, B, b, C, c);
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread writer = new Thread(() -> {
            try { start.await(); for (int i = 0; i < 400; i++) publish(pm, (i & 1) == 0 ? before : after); }
            catch (Throwable error) { failure.set(error); }
        });
        writer.start(); start.countDown();
        PackageManager manager = client(pm);
        for (int i = 0; i < 400; i++) {
            boolean[] result = manager.canPackageQuery(A, new String[]{B, C});
            check(result.length == 2 && result[0] != result[1], "One query mixed package generations");
        }
        writer.join(10_000);
        check(!writer.isAlive() && failure.get() == null, "Concurrent publication failed: " + failure.get());
    }

    private static final class BlockingPackage extends PackageImpl {
        final CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        PackageManagerService owner;
        volatile boolean block;
        BlockingPackage(String name) { super(name, "/fixture/base.apk", "/fixture/base.apk", null, false, null); }
        @Override public List<String> getProtectedBroadcasts() {
            if (block) {
                if (Thread.holdsLock(owner)) throw new AssertionError("Public visibility construction held PMS monitor");
                entered.countDown();
                try {
                    if (!release.await(10, TimeUnit.SECONDS)) throw new AssertionError("Publication fixture timed out");
                } catch (InterruptedException failure) { throw new AssertionError(failure); }
            }
            return super.getProtectedBroadcasts();
        }
    }
    private static void publicationDuringConstruction() throws Exception {
        BlockingPackage original = new BlockingPackage(A);
        original.setTargetSdkVersion(36); original.addQueriesPackage(B);
        ApkPackage a = record(original, 68_001), b = record(parsed(B), 68_002);
        Map<String, ApkPackage> records = new LinkedHashMap<>(); records.put(A, a); records.put(B, b);
        for (int i = 0; i < 30; i++) {
            ApkPackage filler = record(parsed("fixture.visibility.blocked.p" + i), 68_010 + i);
            records.put(filler.packageName, filler);
        }
        PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
        ApkPackage replacement = record(parsed(A), 68_003);
        Map<String, ApkPackage> replaced = new LinkedHashMap<>(records); replaced.put(A, replacement);
        PackageManager manager = client(pm);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<Boolean> result = new AtomicReference<>();
        CountDownLatch published = new CountDownLatch(1);
        original.owner = pm; original.block = true;
        Thread reader = new Thread(() -> {
            try { as(android.os.Process.SYSTEM_UID, () -> result.set(manager.canPackageQuery(A, B))); }
            catch (Throwable error) { failure.set(error); }
        });
        Thread writer = new Thread(() -> {
            try { publish(pm, replaced); published.countDown(); }
            catch (Throwable error) { failure.set(error); }
        });
        reader.start();
        try {
            check(original.entered.await(5, TimeUnit.SECONDS), "Reader never entered snapshot construction: " + failure.get());
            writer.start();
            check(published.await(5, TimeUnit.SECONDS), "Snapshot construction blocked package publication");
            check(get(pm, "mVisibilitySnapshot") == null, "Publication retained in-flight snapshot metadata");
            check(pm.getNameForUid(68_001) == null && A.equals(pm.getNameForUid(68_003)),
                    "UID lookup waited for or borrowed a retired visibility construction");
            check(get(pm, "mVisibilitySnapshot") == null, "UID lookup constructed a replacement visibility view");
        } finally {
            original.block = false; original.release.countDown();
            reader.join(10_000); if (writer.getState() != Thread.State.NEW) writer.join(10_000);
        }
        check(!reader.isAlive() && !writer.isAlive() && failure.get() == null && Boolean.TRUE.equals(result.get()),
                "Captured query lost coherent old-generation behavior: " + failure.get());
        check(!manager.canPackageQuery(A, B), "A stale construction was published after its generation retired");
        check(pm.getPackagesForUid(68_001) == null
                        && Arrays.equals(pm.getPackagesForUid(68_003), new String[]{A}),
                "Retired constructor resurrected UID ownership");
    }

    private static final String UID_PREFIX = "fixture.visibility.uid.";
    private static final String UID_APP_PREFIX = UID_PREFIX + "p";
    private static final String UID_GROUP = "fixture.visibility.uid.group";
    private static Map<String, ApkPackage> uidRecords() throws Exception {
        Map<String, ApkPackage> result = new LinkedHashMap<>();
        for (int i = 0; i < 32; i++) {
            PackageImpl value = parsed(UID_APP_PREFIX + i);
            if (i < 2) value.setSharedUserId(UID_GROUP);
            permission(value, Manifest.permission.CAMERA);
            int uid = i < 2 ? 30_000 : i == 2 ? 130_000 : i == 3 ? 30_001 : 30_000 + i;
            ApkPackage code = record(value, uid);
            // This fixture supplies parsed unsigned metadata, not an APK verifier.
            set(code, "mSigningIdentityChecked", true);
            result.put(value.getPackageName(), code);
        }
        return result;
    }

    private static String uidName(int uid, boolean bulk) {
        if (uid == android.os.Process.SYSTEM_UID) return bulk ? "shared:android.uid.system" : "android.uid.system:1000";
        if (uid == android.os.Process.SHELL_UID) return bulk ? "shared:android.uid.shell" : "android.uid.shell:2000";
        if (uid == 30_000) return bulk ? "shared:" + UID_GROUP : UID_GROUP + ":30000";
        if (uid == 130_000) return UID_APP_PREFIX + 2;
        if (uid == 30_001) return UID_APP_PREFIX + 3;
        return uid >= 30_004 && uid < 30_032 ? UID_APP_PREFIX + (uid - 30_000) : null;
    }

    private static String[] uidPackages(int uid) {
        if (uid == 30_000) return new String[]{UID_APP_PREFIX + 0, UID_APP_PREFIX + 1};
        if (uid == android.os.Process.SYSTEM_UID) return new String[]{"android"};
        if (uid == android.os.Process.SHELL_UID) return new String[]{"com.android.shell"};
        String name = uidName(uid, false);
        return name == null ? null : new String[]{name};
    }

    private static void uidShapesAndLifetimes() throws Exception {
        int[] examples = {30_000, 130_000, 230_000, 30_001, 30_002, 30_031,
                android.os.Process.SYSTEM_UID, android.os.Process.SHELL_UID, -1,
                Integer.MIN_VALUE, Integer.MAX_VALUE, 0, android.os.Process.myUid()};
        for (int warm = 0; warm < 4; warm++) {
            Map<String, ApkPackage> records = uidRecords();
            PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
            if ((warm & 1) != 0) check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "UID public warm-up failed");
            if ((warm & 2) != 0) check(serviceQuery(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "UID installed warm-up failed");
            Object snapshot = get(pm, "mVisibilitySnapshot");
            Object publicView = snapshot == null ? null : get(snapshot, "publicPackages");
            Object installedView = snapshot == null ? null : get(snapshot, "installedPackages");
            check(pm.getNamesForUids(null) == null && pm.getNamesForUids(new int[0]) == null,
                    "Absent UID input changed shape");
            for (int uid : examples) {
                check(java.util.Objects.equals(pm.getNameForUid(uid), uidName(uid, false)),
                        "Wrong exact singular UID identity for " + uid + ", warm=" + warm);
                String[] first = pm.getPackagesForUid(uid);
                check(Arrays.equals(first, uidPackages(uid)), "Wrong exact ordered UID owners for " + uid);
                if (first != null) {
                    first[0] = "caller mutation";
                    check(Arrays.equals(pm.getPackagesForUid(uid), uidPackages(uid)), "UID owner array was shared");
                }
            }
            for (int size : new int[]{1, 4, 15, 16, 17, 128}) {
                int[] input = new int[size]; String[] expected = new String[size];
                for (int i = 0; i < size; i++) {
                    input[i] = examples[(size - i + 3) % examples.length];
                    expected[i] = uidName(input[i], true);
                }
                int[] original = input.clone();
                String[] result = pm.getNamesForUids(input);
                check(Arrays.equals(result, expected), "UID batch lost names, duplicate positions or unknowns");
                check(Arrays.equals(input, original), "UID batch sorted or otherwise mutated caller input");
                result[0] = "caller mutation";
                check(Arrays.equals(pm.getNamesForUids(input), expected), "UID name array was shared");
            }
            int[] reverse = new int[128]; String[] expectedReverse = new String[128];
            for (int i = 0; i < reverse.length; i++) {
                reverse[i] = 30_127 - i; expectedReverse[i] = uidName(reverse[i], true);
            }
            check(Arrays.equals(pm.getNamesForUids(reverse), expectedReverse), "Reverse UID batch lost first matches");
            check(get(pm, "mVisibilitySnapshot") == snapshot, "UID calls created or replaced a visibility snapshot");
            if (snapshot != null) {
                check(get(snapshot, "publicPackages") == publicView && get(snapshot, "installedPackages") == installedView,
                        "UID calls built an unused visibility view");
            }

            ApkPackage first = records.get(UID_APP_PREFIX + 0);
            Method begin = method(PackageManagerService.class, "beginPackageRemoval", ApkPackage.class);
            Method end = method(PackageManagerService.class, "endPackageRemoval", ApkPackage.class);
            invoke(begin, pm, first);
            check(query(pm, UID_APP_PREFIX + 1, UID_APP_PREFIX + 1), "Pending-removal view could not be constructed");
            check(Arrays.equals(pm.getPackagesForUid(30_000), uidPackages(30_000)),
                    "Pending removal prematurely removed an admitted UID owner");
            check(uidName(30_000, false).equals(pm.getNameForUid(30_000)), "Pending removal changed first UID name");
            invoke(end, pm, first);
            check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "UID removal rollback failed");
            invoke(begin, pm, first);
            invoke(method(PackageManagerService.class, "recoveredPackageRemovals"), pm);
            check(Arrays.equals(pm.getPackagesForUid(30_000), uidPackages(30_000)), "Recovered removal lost UID ownership");
            check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "Recovered UID view warm-up failed");
            Object retired = get(pm, "mVisibilitySnapshot");

            Map<String, ApkPackage> replacement = new LinkedHashMap<>(records);
            replacement.remove(UID_APP_PREFIX + 0); replacement.remove(UID_APP_PREFIX + 1);
            publish(pm, replacement);
            // Deterministically model a reader that acquired the old cache before
            // publication; the getter must validate its captured generation.
            set(pm, "mVisibilitySnapshot", retired);
            check(pm.getNameForUid(30_000) == null && pm.getPackagesForUid(30_000) == null,
                    "Retired UID remained in identity metadata");
            set(pm, "mVisibilitySnapshot", null);
            ApkPackage reused = record(parsed(UID_PREFIX + "replacement"), 30_000);
            replacement = new LinkedHashMap<>(replacement); replacement.put(reused.packageName, reused);
            publish(pm, replacement);
            check(query(pm, reused.packageName, reused.packageName), "Replacement metadata warm-up failed");
            check(reused.packageName.equals(pm.getNameForUid(30_000))
                            && Arrays.equals(pm.getPackagesForUid(30_000), new String[]{reused.packageName}),
                    "Reused UID inherited an old owner or shared name");
            check((UID_APP_PREFIX + 2).equals(pm.getNameForUid(130_000)), "Reused app-ID changed another exact UID");
        }

        // Metadata fixtures explicitly cover insertion order even when a real APK
        // replaces the reserved host definition in the visibility table.
        Map<String, ApkPackage> withAndroid = uidRecords();
        withAndroid.put("android", record(parsed("android"), 30_000));
        PackageManagerService reserved = service(withAndroid.values().toArray(new ApkPackage[0]));
        check(query(reserved, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "Reserved-name metadata warm-up failed");
        check(uidName(30_000, false).equals(reserved.getNameForUid(30_000)),
                "Visibility definition order changed the registry's first UID owner");
        check(Arrays.equals(reserved.getPackagesForUid(30_000),
                new String[]{UID_APP_PREFIX + 0, UID_APP_PREFIX + 1, "android"}), "Reserved-name UID order changed");

        Map<String, ApkPackage> legacy = new LinkedHashMap<>();
        for (int i = 0; i < 32; i++) legacy.put(UID_APP_PREFIX + i, record(parsed(UID_APP_PREFIX + i), 40_000));
        PackageManagerService shared = service(legacy.values().toArray(new ApkPackage[0]));
        int[] repeated = new int[64]; String[] repeatedNames = new String[64];
        for (int i = 0; i < repeated.length; i++) {
            repeated[i] = i % 7 == 0 ? android.os.Process.SYSTEM_UID : 40_000;
            repeatedNames[i] = i % 7 == 0 ? "shared:android.uid.system" : UID_APP_PREFIX + 0;
        }
        check(Arrays.equals(shared.getNamesForUids(repeated), repeatedNames),
                "Cold repeated UID batch lost the first owner or core names");
        check(query(shared, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "Legacy UID warm-up failed");
        check((UID_APP_PREFIX + 0).equals(shared.getNameForUid(40_000)), "Legacy UID lost first-name semantics");
        check(Arrays.equals(shared.getPackagesForUid(40_000), legacy.keySet().toArray(new String[0])),
                "Legacy UID lost admitted package iteration order");
    }

    private static void uidLargeGroups() throws Exception {
        Map<String, ApkPackage> records = new LinkedHashMap<>();
        for (int i = 0; i < 1024; i++) {
            int uid = 10_000 + (i * 7919) % 80_000;
            String name = UID_PREFIX + "scatter.p" + i;
            records.put(name, record(parsed(name), uid));
            if (i % 31 == 0) records.put(name + ".user", record(parsed(name + ".user"), uid + 100_000));
        }
        PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
        String source = records.keySet().iterator().next();
        check(query(pm, source, source), "Scattered UID metadata warm-up failed");
        for (ApkPackage code : records.values()) {
            check(code.packageName.equals(pm.getNameForUid(code.uid())), "Growing app-ID index lost an exact UID");
            check(Arrays.equals(pm.getPackagesForUid(code.uid()), new String[]{code.packageName}),
                    "Growing app-ID index merged exact UID owners");
            check(pm.getNameForUid(code.uid() + 300_000) == null,
                    "Scattered app-ID membership leaked into another UID");
        }
    }

    @SuppressWarnings("unchecked")
    private static void uidPermissionLifetimes() throws Exception {
        Map<String, ApkPackage> records = uidRecords();
        PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
        if (!art) {
            PermissionInfo camera = new PermissionInfo(); camera.name = Manifest.permission.CAMERA;
            camera.packageName = "android"; camera.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
            ((Map<String, PermissionInfo>)get(get(pm.permissions(), "mCatalog"), "mFramework"))
                    .put(camera.name, camera);
        }
        check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "Permission metadata warm-up failed");
        Object snapshot = get(pm, "mVisibilitySnapshot");
        as(android.os.Process.SYSTEM_UID, () -> {
            pm.permissions().revokeRuntimePermission(UID_APP_PREFIX + 0, Manifest.permission.CAMERA,
                    "default:0", 0, "fixture reset");
            check(pm.checkUidPermission(Manifest.permission.CAMERA, 30_000) == PackageManager.PERMISSION_DENIED,
                    "Cached identity manufactured a runtime grant");
            pm.permissions().grantRuntime(UID_APP_PREFIX + 0, Manifest.permission.CAMERA, 0);
            check(pm.checkUidPermission(Manifest.permission.CAMERA, 30_000) == PackageManager.PERMISSION_GRANTED,
                    "Cached identity hid a live runtime grant");
            check(pm.checkUidPermission(Manifest.permission.CAMERA, 130_000) == PackageManager.PERMISSION_DENIED
                            && pm.checkUidPermission(Manifest.permission.CAMERA, 230_000) == PackageManager.PERMISSION_DENIED,
                    "App-ID grouping granted another exact UID's permission");
            pm.permissions().revokeRuntimePermission(UID_APP_PREFIX + 0, Manifest.permission.CAMERA,
                    "default:0", 0, "fixture revoke");
            check(pm.checkUidPermission(Manifest.permission.CAMERA, 30_000) == PackageManager.PERMISSION_DENIED,
                    "Cached identity retained a revoked runtime grant");
        });
        check(get(pm, "mVisibilitySnapshot") == snapshot, "Permission decisions rebuilt identity metadata");
    }

    private static void uidMonitorIndependence() throws Exception {
        for (boolean warm : new boolean[]{false, true}) {
            Map<String, ApkPackage> records = uidRecords();
            PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
            if (warm) check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "UID monitor warm-up failed");
            CountDownLatch locked = new CountDownLatch(1), release = new CountDownLatch(1), read = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread holder = new Thread(() -> {
                synchronized (pm) {
                    locked.countDown();
                    try { if (!release.await(10, TimeUnit.SECONDS)) throw new AssertionError("UID holder timeout"); }
                    catch (Throwable error) { failure.set(error); }
                }
            });
            Thread reader = new Thread(() -> {
                try {
                    if (!uidName(30_000, false).equals(pm.getNameForUid(30_000))
                            || !Arrays.equals(pm.getPackagesForUid(30_000), uidPackages(30_000))
                            || !Arrays.equals(pm.getNamesForUids(new int[]{30_000, 230_000}),
                                new String[]{uidName(30_000, true), null})) {
                        throw new AssertionError("UID monitor fixture changed results");
                    }
                    read.countDown();
                } catch (Throwable error) { failure.set(error); }
            });
            holder.start();
            try {
                check(locked.await(5, TimeUnit.SECONDS), "PMS monitor was never acquired");
                reader.start();
                check(read.await(5, TimeUnit.SECONDS), "UID lookup added a PMS monitor dependency: " + failure.get());
            } finally { release.countDown(); holder.join(10_000); reader.join(10_000); }
            check(!holder.isAlive() && !reader.isAlive() && failure.get() == null,
                    "UID monitor fixture failed: " + failure.get());
        }
    }

    private static void uidConcurrentPublication() throws Exception {
        Map<String, ApkPackage> first = new LinkedHashMap<>(), second = new LinkedHashMap<>();
        int[] uids = new int[32];
        for (int i = 0; i < uids.length; i++) {
            uids[i] = 56_000 + i;
            first.put(UID_PREFIX + "old.p" + i, record(parsed(UID_PREFIX + "old.p" + i), uids[i]));
            second.put(UID_PREFIX + "new.p" + i, record(parsed(UID_PREFIX + "new.p" + i), uids[i]));
        }
        PackageManagerService pm = service(first.values().toArray(new ApkPackage[0]));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch start = new CountDownLatch(1);
        Thread writer = new Thread(() -> {
            try {
                start.await();
                for (int i = 0; i < 400; i++) {
                    Map<String, ApkPackage> next = (i & 1) == 0 ? second : first;
                    publish(pm, next);
                    String name = next.keySet().iterator().next();
                    as(android.os.Process.SYSTEM_UID, () -> {
                        if (!query(pm, name, name)) throw new AssertionError("Concurrent UID warm-up failed");
                    });
                }
            } catch (Throwable error) { failure.set(error); }
        });
        writer.start(); start.countDown();
        try {
            for (int i = 0; i < 1000; i++) {
                String[] names = pm.getNamesForUids(uids);
                boolean old = names[0].equals(UID_PREFIX + "old.p0");
                for (int j = 0; j < uids.length; j++) {
                    check(names[j].equals(UID_PREFIX + (old ? "old.p" : "new.p") + j),
                            "One UID batch mixed published generations");
                }
            }
        } finally { writer.join(15_000); }
        check(!writer.isAlive() && failure.get() == null, "UID concurrent publication failed: " + failure.get());
    }

    /** Registry iteration counters are confined to work assertions, never timing. */
    private static final class CountedUidRegistry extends LinkedHashMap<String, ApkPackage> {
        long visits;
        CountedUidRegistry(Map<String, ApkPackage> records) { super(records); }
        @Override public java.util.Collection<ApkPackage> values() {
            java.util.Collection<ApkPackage> original = super.values();
            return new java.util.AbstractCollection<>() {
                @Override public int size() { return original.size(); }
                @Override public java.util.Iterator<ApkPackage> iterator() {
                    java.util.Iterator<ApkPackage> source = original.iterator();
                    return new java.util.Iterator<>() {
                        @Override public boolean hasNext() { return source.hasNext(); }
                        @Override public ApkPackage next() { visits++; return source.next(); }
                    };
                }
            };
        }
    }

    private static void uidWorkContracts() throws Exception {
        Map<String, ApkPackage> records = uidRecords();
        PackageManagerService pm = service(records.values().toArray(new ApkPackage[0]));
        CountedUidRegistry registry = new CountedUidRegistry(records); publish(pm, registry);
        int[] many = new int[128]; String[] expected = new String[many.length];
        for (int i = 0; i < many.length; i++) {
            many[i] = 30_127 - i; expected[i] = uidName(many[i], true);
        }
        registry.visits = 0;
        check(Arrays.equals(pm.getNamesForUids(many), expected), "Cold UID work fixture changed answers");
        check(registry.visits <= records.size(), "Cold UID batch revisited registry records: " + registry.visits);
        check(get(pm, "mVisibilitySnapshot") == null, "Cold UID work fixture caused retained visibility metadata");
        System.out.println("UID_WORK cold_batch=128 packages=" + records.size() + " registry_visits=" + registry.visits);
        check(query(pm, UID_APP_PREFIX + 0, UID_APP_PREFIX + 0), "UID work metadata warm-up failed");
        check(pm.checkUidPermission("fixture.permission.NOT_REQUESTED", 30_031) == PackageManager.PERMISSION_DENIED,
                "UID permission warm-up manufactured a grant");
        registry.visits = 0;
        for (int i = 0; i < 64; i++) {
            check(uidName(30_031, false).equals(pm.getNameForUid(30_031)), "Warm UID scalar work changed answers");
            check(Arrays.equals(pm.getPackagesForUid(30_000), uidPackages(30_000)), "Warm UID owners changed answers");
            check(Arrays.equals(pm.getNamesForUids(many), expected), "Warm UID batch work changed answers");
            check(pm.checkUidPermission("fixture.permission.NOT_REQUESTED", 30_031) == PackageManager.PERMISSION_DENIED,
                    "Warm public UID permission path manufactured a grant");
        }
        check(registry.visits == 0, "Warm UID queries rescanned unrelated registry records: " + registry.visits);
        System.out.println("UID_WORK warm_queries=256 packages=" + records.size() + " registry_visits=" + registry.visits);
    }

    public static int verifyUidSemantics() throws Exception {
        art = false; checks = 0;
        uidShapesAndLifetimes(); uidLargeGroups(); uidPermissionLifetimes(); uidMonitorIndependence();
        return checks;
    }
    public static int verifyUidWork() throws Exception {
        art = false; checks = 0; uidWorkContracts(); return checks;
    }
    public static Object[] uidMetadataStorage(int count, boolean grouped) throws Exception {
        UidWorkload work = new UidWorkload(count, grouped ? "uid-warm-group" : "uid-warm-name", 1);
        Object snapshot = get(work.pm, "mVisibilitySnapshot");
        return new Object[]{snapshot, get(snapshot, "publicPackages")};
    }

    /** Actual public PMS identity/permission calls, with no counted metadata or substitute resolver. */
    public static final class UidWorkload {
        final PackageManagerService pm;
        final String operation, expectedName;
        final String[] expectedOwners, expectedBulk;
        final int uid;
        final int[] batch;
        public UidWorkload(int count, String kind, int batchSize) throws Exception {
            if (count < 1 || batchSize < 1) throw new IllegalArgumentException("Positive workload sizes required");
            String[] pieces = kind.split("-");
            if (pieces.length != 3 || !pieces[0].equals("uid")) throw new IllegalArgumentException(kind);
            operation = pieces[2];
            boolean group = operation.equals("group");
            Map<String, ApkPackage> installed = new LinkedHashMap<>();
            for (int i = 0; i < count; i++) {
                String name = UID_PREFIX + "work.p" + i;
                PackageImpl value = parsed(name); permission(value, Manifest.permission.ACCESS_NETWORK_STATE);
                installed.put(name, record(value, group ? 50_000 : 50_000 + i));
            }
            pm = service(installed.values().toArray(new ApkPackage[0]));
            PermissionInfo definition = new PermissionInfo(); definition.name = Manifest.permission.ACCESS_NETWORK_STATE;
            definition.packageName = "android"; definition.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
            @SuppressWarnings("unchecked") Map<String, PermissionInfo> definitions =
                    (Map<String, PermissionInfo>)get(get(pm.permissions(), "mCatalog"), "mFramework");
            definitions.put(definition.name, definition);
            uid = group ? 50_000 : 50_000 + count - 1;
            expectedName = UID_PREFIX + "work.p" + (group ? 0 : count - 1);
            expectedOwners = group ? installed.keySet().toArray(new String[0]) : new String[]{expectedName};
            batch = new int[batchSize]; expectedBulk = new String[batchSize];
            for (int i = 0; i < batchSize; i++) {
                if (operation.equals("core")) {
                    batch[i] = (i & 1) == 0 ? android.os.Process.SYSTEM_UID : android.os.Process.SHELL_UID;
                    expectedBulk[i] = (i & 1) == 0 ? "shared:android.uid.system" : "shared:android.uid.shell";
                } else if (operation.equals("repeated")) {
                    batch[i] = 50_000; expectedBulk[i] = UID_PREFIX + "work.p0";
                } else {
                    int index = count - 1 - i % count;
                    boolean unknown = i % 7 == 0;
                    batch[i] = unknown ? 900_000 + i : 50_000 + index;
                    expectedBulk[i] = unknown ? null : UID_PREFIX + "work.p" + index;
                }
            }
            if (pieces[1].equals("warm")) {
                String first = installed.keySet().iterator().next();
                if (!query(pm, first, first)) throw new AssertionError("UID timing metadata warm-up failed");
            } else if (!pieces[1].equals("cold")) throw new IllegalArgumentException(kind);
        }
        public void run(int iterations) {
            for (int i = 0; i < iterations; i++) {
                switch (operation) {
                    case "name" -> {
                        if (!expectedName.equals(pm.getNameForUid(uid))) throw new AssertionError("Wrong measured UID name");
                    }
                    case "owners", "group" -> {
                        if (!Arrays.equals(expectedOwners, pm.getPackagesForUid(uid))) throw new AssertionError("Wrong measured UID owners");
                    }
                    case "permission" -> {
                        if (pm.checkUidPermission(Manifest.permission.ACCESS_NETWORK_STATE, uid)
                                != PackageManager.PERMISSION_GRANTED) throw new AssertionError("Wrong measured UID permission");
                    }
                    case "bulk", "core", "repeated" -> {
                        if (!Arrays.equals(expectedBulk, pm.getNamesForUids(batch))) throw new AssertionError("Wrong measured UID batch");
                    }
                    default -> throw new AssertionError("Unknown UID workload " + operation);
                }
            }
        }
    }

    private static long metadataReads;
    private static final class CountedPackage extends PackageImpl {
        CountedPackage(String name) { super(name, "/fixture/base.apk", "/fixture/base.apk", null, false, null); }
        @Override public int getTargetSdkVersion() { metadataReads++; return super.getTargetSdkVersion(); }
        @Override public List<String> getProtectedBroadcasts() { metadataReads++; return super.getProtectedBroadcasts(); }
    }

    /** Complete public queries over constructed admitted metadata; no matcher is replaced. */
    public static final class Workload {
        final PackageManagerService pm;
        final PackageManager manager;
        final Map<String, ApkPackage> installed = new LinkedHashMap<>();
        final Method publish = method(PackageManagerService.class, "publishPackages", Map.class);
        final String source = "fixture.visibility.work.p0";
        final String[] targets;
        public final int expectedVisible;
        final boolean cold;
        public Workload(int count, String kind, boolean counted) throws Exception {
            if (count < 2) throw new IllegalArgumentException("At least two package records are required");
            cold = kind.equals("cold");
            boolean bulk = kind.equals("bulk"), hidden = kind.equals("hidden") || kind.equals("groups");
            targets = new String[bulk ? count - 1 : 1];
            for (int i = 0; i < count; i++) {
                String name = "fixture.visibility.work.p" + i;
                PackageImpl parsed = counted ? new CountedPackage(name) : parsed(name);
                parsed.setTargetSdkVersion(36); parsed.setEnabled(true);
                if (i == 0 && !hidden) {
                    if (bulk) permission(parsed, Manifest.permission.QUERY_ALL_PACKAGES);
                    else parsed.addQueriesPackage("fixture.visibility.work.p" + (count - 1));
                }
                int uid = kind.equals("groups") ? 20_000 + (i < count / 2 ? 0 : 1) : 20_000 + i;
                installed.put(name, record(parsed, uid));
                if (bulk && i != 0) targets[i - 1] = name;
            }
            if (!bulk) targets[0] = "fixture.visibility.work.p" + (count - 1);
            expectedVisible = hidden ? 0 : targets.length;
            pm = service(installed.values().toArray(new ApkPackage[0]));
            manager = client(pm);
        }
        public void run(int iterations) throws Exception {
            long visible = 0;
            for (int i = 0; i < iterations; i++) {
                if (cold) synchronized (pm) { invoke(publish, pm, installed); }
                boolean[] result = manager.canPackageQuery(source, targets);
                if (result.length != targets.length) throw new AssertionError("Query lost positional results");
                for (boolean value : result) if (value) visible++;
            }
            if (visible != (long)iterations * expectedVisible) throw new AssertionError("Query changed visible results");
        }
    }
    private static void workContracts() throws Exception {
        for (int count : new int[]{2, 50, 500}) {
            Workload workload = new Workload(count, "selective", true);
            metadataReads = 0; workload.run(1);
            long cold = metadataReads;
            metadataReads = 0; workload.run(64);
            check(cold >= count * 2 && metadataReads == 0,
                    "Repeated query re-read unrelated admitted metadata: " + metadataReads);
            System.out.println("VISIBILITY_WORK packages=" + count + " queries=64 cold_metadata_reads="
                    + cold + " warm_metadata_reads=" + metadataReads);
        }
    }
    public static int verifyWork() throws Exception {
        art = false; checks = 0; workContracts(); return checks;
    }
    public static int run(boolean onArt) throws Exception {
        art = onArt; checks = 0; shapesAndCallers();
        installSourceVisibility();
        authenticatedServiceVisibility();
        declarations(); components();
        snapshotLifetimes(); liveResourcePolicy(); concurrentPublication(); publicationDuringConstruction();
        uidShapesAndLifetimes(); uidLargeGroups(); uidPermissionLifetimes(); uidMonitorIndependence(); uidConcurrentPublication();
        if (!art) uidWorkContracts();
        if (!art) workContracts();
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
