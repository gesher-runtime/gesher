/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.IDexModuleRegisterCallback;
import android.os.RemoteException;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import sun.misc.Unsafe;

/** Host/ART production-method checks; real PackageManager/Parcel tests live beside these. */
public final class PackageServiceQueriesTest {
    private static int checks;

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void field(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static final class QueueExecutor implements Executor {
        final ArrayDeque<Runnable> work = new ArrayDeque<>();
        @Override public void execute(Runnable task) { work.addLast(task); }
        void runOne() { work.removeFirst().run(); }
    }

    private static ApkPackage record(Unsafe unsafe, String name, int uid) throws Exception {
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(pkg, "packageName", name);
        field(pkg, "mUid", uid);
        return pkg;
    }

    public static void main(String[] args) throws Exception {
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        Unsafe unsafe = (Unsafe) singleton.get(null);
        // The methods below use package identities and an executor, not the
        // native-backed installer/permission services built by the constructor.
        PackageManagerService service = (PackageManagerService) unsafe.allocateInstance(
                PackageManagerService.class);
        Map<String, ApkPackage> packages = new LinkedHashMap<>();
        packages.put("fixture.one", record(unsafe, "fixture.one", 12001));
        packages.put("fixture.two", record(unsafe, "fixture.two", 12002));
        ApkPackage addon = record(unsafe, "fixture.system", 12003);
        field(addon, "mSystemAddon", true);
        packages.put("fixture.system", addon);
        field(service, "mPackages", packages);
        field(service, "mRemoving", new LinkedHashMap<String, ApkPackage>());
        QueueExecutor executor = new QueueExecutor();
        field(service, "mPackageEventExecutor", executor);

        require(service.getPackageGids("fixture.one", 0, 0).length == 0
                && service.getPackageGids("fixture.two", 0, 0).length == 0,
                "Known packages became unknown when querying supplementary groups");
        require(service.getPackageGids("android", 0, 0).length == 0,
                "Platform package lost its group inventory");
        require(service.getPackageGids("fixture.one", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0) == null,
                "System-only supplementary-group query included an ordinary package");
        require(service.getPackageGids("fixture.system", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0).length == 0
                        && service.getPackageGids("android", android.content.pm.PackageManager.MATCH_SYSTEM_ONLY, 0).length == 0,
                "System-only supplementary-group query excluded an addon or the platform");
        require(service.getPackageGids("fixture.absent", 0, 0) == null
                && service.getPackageGids("fixture.one", 0, 10) == null
                && service.getPackageGids("com.android.vending", 0, 0) == null,
                "Unknown package/user acquired groups");
        int[] uids = {12002, -1, 12001, 1000, 12001};
        String[] expectedNames = {"fixture.two", null, "fixture.one", "shared:android.uid.system", "fixture.one"};
        require(Arrays.equals(service.getNamesForUids(uids), expectedNames),
                "Bulk UID query lost request positions, duplicates or unknown names");
        String[] changed = service.getNamesForUids(uids);
        changed[0] = "caller.changed";
        require(Arrays.equals(service.getNamesForUids(uids), expectedNames),
                "Caller mutation changed the UID name inventory");
        require(service.getNamesForUids(new int[0]) == null && service.getNamesForUids(null) == null,
                "Absent UID input did not retain Android's null contract");
        packages.remove("fixture.one");
        require(service.getPackageGids("fixture.one", 0, 0) == null
                && Arrays.equals(service.getNamesForUids(new int[]{12001}), new String[]{null}),
                "Removed package remained in identity queries");

        List<String> calls = new ArrayList<>();
        IDexModuleRegisterCallback callback = new IDexModuleRegisterCallback.Default() {
            @Override public void onDexModuleRegistered(String path, boolean success, String message) {
                require(!Thread.holdsLock(service), "Callback ran with the package monitor held");
                require(!success && "registerDexModule call not supported since Android U".equals(message),
                        "Unsupported registration claimed success or lost its reason");
                calls.add(path);
                if ("first.dex".equals(path)) {
                    service.registerDexModule("fixture.two", "reentrant.dex", false, this);
                    require(calls.size() == 1, "Reentrant callback ran inline on the caller");
                }
            }
        };
        service.registerDexModule("fixture.one", "first.dex", false, callback);
        service.registerDexModule("fixture.two", "second.dex", true, callback);
        service.registerDexModule("fixture.one", "no-callback.dex", true, null);
        require(calls.isEmpty() && executor.work.size() == 2,
                "Registration failed to schedule exactly one asynchronous completion per callback");
        executor.runOne();
        require(calls.equals(Arrays.asList("first.dex")) && executor.work.size() == 2,
                "First completion lost identity or the queued reentrant request");
        executor.runOne();
        executor.runOne();
        require(calls.equals(Arrays.asList("first.dex", "second.dex", "reentrant.dex"))
                && executor.work.isEmpty(), "Callbacks were reordered, duplicated or omitted");

        int[] failedCalls = {0};
        service.registerDexModule("fixture.absent", "dead-client.dex", false,
                new IDexModuleRegisterCallback.Default() {
                    @Override public void onDexModuleRegistered(String path, boolean success, String message)
                            throws RemoteException {
                        failedCalls[0]++;
                        throw new RemoteException("fixture disconnected");
                    }
                });
        service.registerDexModule("fixture.two", "bad-local-client.dex", false,
                new IDexModuleRegisterCallback.Default() {
                    @Override public void onDexModuleRegistered(String path, boolean success, String message) {
                        failedCalls[0]++;
                        throw new IllegalStateException("fixture callback failed");
                    }
                });
        service.registerDexModule("fixture.two", "after-failure.dex", false, callback);
        executor.runOne();
        executor.runOne();
        executor.runOne();
        require(failedCalls[0] == 2 && calls.size() == 4 && "after-failure.dex".equals(calls.get(3))
                && executor.work.isEmpty(), "A failed callback was retried or blocked later completions");
        System.out.println("PackageServiceQueriesTest all passed: " + checks + " checks");
    }
}
