/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IUriGrantsManager;
import android.app.GrantedUriPermission;
import android.content.Intent;
import android.content.UriPermission;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;
import com.gesher.server.UriGrants;
import com.gesher.server.UriGrantsManagerService;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Contract tests for F11: Persistable URI Grants Across Runtime Lifecycles. */
public final class PersistableUriGrantContractTest {
    private static final String OWNER = "fixture.uri.owner";
    private static final String CLIENT = "fixture.uri.client";
    private static final String OTHER = "fixture.uri.other";
    private static final String PEER = "fixture.uri.shared.peer";
    private static final int OWNER_UID = 10100;
    private static final int CLIENT_UID = 10200;
    private static final int OTHER_UID = 10300;
    private static final String AUTHORITY = "fixture.uri.provider";
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int PERSISTABLE = Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;

    private static Unsafe unsafe;
    private static int checks = 0;
    private static boolean binder;

    private interface Operation { void run() throws Exception; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void set(Object target, String name, Object value) throws Exception {
        Class<?> current = target.getClass();
        while (current != null) {
            try {
                Field f = current.getDeclaredField(name);
                f.setAccessible(true);
                f.set(target, value);
                return;
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static IBinder token() {
        if (binder) return new Binder();
        return (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                (self, method, args) -> switch (method.getName()) {
                    case "equals" -> self == args[0];
                    case "hashCode" -> System.identityHashCode(self);
                    case "toString" -> "Host URI owner token";
                    default -> throw new AssertionError("Unexpected host Binder operation: " + method.getName());
                });
    }

    private static final class ThreadToken extends IApplicationThread.Default {
        private final IBinder token = token();
        @Override public IBinder asBinder() { return token; }
    }

    private static ApkPackage createPackage(String name, int uid, boolean withProvider) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setTargetSdkVersion(36);
        parsed.setEnabled(true);
        if (withProvider) {
            ParsedPermissionImpl rPerm = new ParsedPermissionImpl();
            rPerm.setName(name + ".READ"); rPerm.setPackageName(name);
            rPerm.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL); parsed.addPermission(rPerm);
            ParsedPermissionImpl wPerm = new ParsedPermissionImpl();
            wPerm.setName(name + ".WRITE"); wPerm.setPackageName(name);
            wPerm.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL); parsed.addPermission(wPerm);

            ParsedProviderImpl provider = new ParsedProviderImpl();
            provider.setName(name + ".Provider");
            provider.setPackageName(name);
            provider.setEnabled(true);
            provider.setExported(true);
            provider.setAuthority(AUTHORITY);
            provider.setGrantUriPermissions(true);
            provider.setReadPermission(name + ".READ");
            provider.setWritePermission(name + ".WRITE");
            parsed.addProvider(provider);
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(code, "packageName", name);
        set(code, "parsed", parsed);
        set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk");
        set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", SharedLibraryGraph.EMPTY);
        set(code, "mEnabledOwner", "fixture-install-owner:" + name);
        return code;
    }

    private static final class Harness {
        final java.util.ArrayDeque<Runnable> packageEvents = new java.util.ArrayDeque<>();
        final Map<String, ApkPackage> records = new LinkedHashMap<>();
        final Map<String, ThreadToken> threads = new LinkedHashMap<>();
        final Map<String, Integer> pids = new LinkedHashMap<>();
        final File stateFile;
        PackageManagerService pm;
        ActivityTaskManagerService atm;
        UriGrants grants;
        ActivityManagerService amService;
        IActivityManager am;
        UriGrantsManagerService ugmService;
        IUriGrantsManager ugm;
        final Object providers;

        Harness(File stateFile) throws Exception {
            this(stateFile, false);
        }

        Harness(File stateFile, boolean hostedProviders) throws Exception {
            this.stateFile = stateFile;
            records.put(OWNER, createPackage(OWNER, OWNER_UID, true));
            records.put(CLIENT, createPackage(CLIENT, CLIENT_UID, false));
            records.put(OTHER, createPackage(OTHER, OTHER_UID, false));
            // The fixture seeds already-admitted identities. PackageUidIsolationContractTest
            // separately exercises signed admission into this shared-UID state.
            records.put(PEER, createPackage(PEER, CLIENT_UID, false));

            pm = (PackageManagerService) unsafe.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", new LinkedHashMap<>(records));
            set(pm, "mRemoving", new LinkedHashMap<String, ApkPackage>());
            set(pm, "mPackageRemoval", new ArrayList<>());
            set(pm, "mPackageEventExecutor", (java.util.concurrent.Executor)packageEvents::addLast);
            Class<?> policy = Class.forName("com.gesher.server.PermissionPolicy");
            Constructor<?> policyConstructor = policy.getDeclaredConstructor(PackageManagerService.class);
            policyConstructor.setAccessible(true);
            set(pm, "mPermissions", policyConstructor.newInstance(pm));

            atm = (ActivityTaskManagerService) unsafe.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm);
            List<Object> processes = new ArrayList<>();
            set(atm, "mProcesses", processes);
            Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
            int pid = 911001;
            for (String name : records.keySet()) {
                ThreadToken thread = new ThreadToken(); threads.put(name, thread); pids.put(name, pid);
                Object process = unsafe.allocateInstance(processType);
                set(process, "pid", pid++); set(process, "uid", records.get(name).uid());
                set(process, "thread", thread); set(process, "packageName", name);
                set(process, "processName", name); set(process, "ready", true);
                processes.add(process);
            }

            Class<?> providersType = Class.forName("com.gesher.server.ContentProviders");
            providers = unsafe.allocateInstance(providersType);
            set(providers, "mPm", pm);
            set(providers, "mRegisteredProviders", Map.of());
            if (hostedProviders) {
                Constructor<UriGrants> constructor = UriGrants.class.getDeclaredConstructor(
                        PackageManagerService.class, ActivityTaskManagerService.class, providersType, File.class);
                constructor.setAccessible(true);
                grants = constructor.newInstance(pm, atm, providers, stateFile);
            } else {
                grants = new UriGrants(pm, atm, stateFile);
            }
            set(providers, "mPm", pm); set(providers, "mUriGrants", grants);
            amService = (ActivityManagerService) unsafe.allocateInstance(ActivityManagerService.class);
            set(amService, "mPm", pm); set(amService, "mAtm", atm); set(amService, "mProviders", providers);
            set(amService, "mCallers", new CallerIdentity(pm, atm));

            am = amService;
            ugmService = new UriGrantsManagerService(pm, atm, grants);
            ugm = ugmService;
        }

        void as(String name, Operation operation) throws Exception {
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) records.get(name).uid() << 32) | pids.get(name));
                operation.run();
            } finally {
                Binder.restoreCallingIdentity(previous);
            }
        }

        void grant(String sender, String target, Uri uri, int modes) throws Exception {
            as(sender, () -> am.grantUriPermission(threads.get(sender), target, uri, modes, 0));
        }

        void take(String caller, Uri uri, int modes) throws Exception {
            as(caller, () -> ugm.takePersistableUriPermission(uri, modes, null, 0));
        }

        void release(String caller, Uri uri, int modes) throws Exception {
            as(caller, () -> ugm.releasePersistableUriPermission(uri, modes, null, 0));
        }

        @SuppressWarnings("unchecked")
        List<GrantedUriPermission> getGranted(String target) throws Exception {
            return (List<GrantedUriPermission>) ugm.getGrantedUriPermissions(target, 0).getList();
        }

        void clearTransient(String packageName) throws Exception {
            var clear = UriGrants.class.getDeclaredMethod("clearTransient", String.class);
            clear.setAccessible(true);
            clear.invoke(grants, packageName);
        }

        void owned(IBinder owner, String target, Uri uri, int modes) throws Exception {
            var prepare = UriGrants.class.getDeclaredMethod("prepareIntent", int.class, String.class, Intent.class);
            var publish = UriGrants.class.getDeclaredMethod("publish", IBinder.class, List.class);
            prepare.setAccessible(true); publish.setAccessible(true);
            Object prepared = prepare.invoke(grants, OWNER_UID, target, new Intent().setData(uri).addFlags(modes));
            publish.invoke(grants, owner, prepared);
        }

        void retire(IBinder owner) throws Exception {
            var retire = UriGrants.class.getDeclaredMethod("revokeByOwner", IBinder.class);
            retire.setAccessible(true); retire.invoke(grants, owner);
        }

        void replace(String name) throws Exception {
            ApkPackage previous = records.get(name);
            ApkPackage replacement = createPackage(name, previous.uid(), OWNER.equals(name));
            records.put(name, replacement);
            // This models the metadata publication after identity admission, not an APK install.
            var publish = PackageManagerService.class.getDeclaredMethod("publishPrepared", ApkPackage.class);
            int queued = packageEvents.size();
            publish.setAccessible(true); publish.invoke(pm, replacement);
            if (packageEvents.size() != queued + 1) {
                throw new AssertionError("Replacement did not queue its package lifecycle callback");
            }
            // Execute the real production callbacks after publication releases its locks.
            // A no-op executor would hide persisted-grant retirement on replacement.
            while (!packageEvents.isEmpty()) packageEvents.removeFirst().run();
        }

        void removeCapabilities(String name) throws Exception {
            var remove = PackageManagerService.class.getDeclaredMethod("revokePackageCapabilities", String.class);
            remove.setAccessible(true); remove.invoke(pm, name);
        }

        @SuppressWarnings("unchecked")
        List<UriPermission> getPersisted(String caller, boolean incoming) throws Exception {
            final List<UriPermission>[] holder = new List[1];
            as(caller, () -> holder[0] = (List<UriPermission>) ugm.getUriPermissions(caller, incoming, true).getList());
            return holder[0];
        }

        boolean canAccess(String target, Uri uri, int modes) {
            return grants.canAccess(records.get(target), uri, modes, 0);
        }

        int checkUri(String caller, Uri uri, int modes) throws Exception {
            int[] result = new int[1];
            as(caller, () -> result[0] = am.checkUriPermission(uri, pids.get(caller),
                    records.get(caller).uid(), modes, 0, null));
            return result[0];
        }

        void rejected(Class<? extends Throwable> expected, Operation op) {
            try {
                op.run();
                throw new AssertionError("Expected " + expected.getSimpleName() + " was not thrown");
            } catch (Throwable t) {
                if (expected.isInstance(t)) {
                    checks++;
                } else if (t instanceof InvocationTargetException && expected.isInstance(t.getCause())) {
                    checks++;
                } else {
                    throw new AssertionError("Wrong exception: expected " + expected + ", got " + t, t);
                }
            }
        }
    }

    private static void registeredProviderBatch() throws Exception {
        Harness h = new Harness(null, true);
        android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
        info.packageName = "android";
        info.name = "FixtureHostedProvider";
        info.applicationInfo = h.pm.getApplicationInfo("android", 0, 0);
        info.authority = "fixture.hosted.provider";
        info.grantUriPermissions = true;
        Class<?> registrationType = Class.forName("com.gesher.server.ContentProviders$RegisteredProvider");
        var constructor = registrationType.getDeclaredConstructor(
                android.content.pm.ProviderInfo.class, android.content.IContentProvider.class);
        constructor.setAccessible(true);
        // A transport boundary fixture: registration/grant code is real; this
        // controlled transport does not claim document IO or provider Binder IPC.
        Object transport = Proxy.newProxyInstance(android.content.IContentProvider.class.getClassLoader(),
                new Class<?>[]{android.content.IContentProvider.class}, (self, method, arguments) -> {
                    throw new AssertionError("Provider IO is outside this grant fixture: " + method.getName());
                });
        Object registration = constructor.newInstance(info, transport);
        var register = h.providers.getClass().getDeclaredMethod("registerProvider", registrationType);
        register.setAccessible(true); register.invoke(h.providers, registration);
        var prepare = UriGrants.class.getDeclaredMethod("prepareFromProvider", registrationType,
                String.class, Uri.class, int.class);
        prepare.setAccessible(true);
        Uri first = Uri.parse("content://fixture.hosted.provider/selected/one");
        Uri second = Uri.parse("content://fixture.hosted.provider/selected/two");
        List<Object> selection = new ArrayList<>();
        selection.add(prepare.invoke(h.grants, registration, CLIENT, first, READ | PERSISTABLE));
        selection.add(prepare.invoke(h.grants, registration, CLIENT, second, READ | PERSISTABLE));
        check(!h.canAccess(CLIENT, first, READ) && !h.canAccess(CLIENT, second, READ),
                "Preparing a hosted-provider selection publishes no permission");
        IBinder owner = token();
        var publish = UriGrants.class.getDeclaredMethod("publish", IBinder.class, List.class);
        publish.setAccessible(true); publish.invoke(h.grants, owner, selection);
        check(h.canAccess(CLIENT, first, READ) && h.canAccess(CLIENT, second, READ),
                "A registered hosted provider publishes its complete owner-scoped selection");
        h.take(CLIENT, first, READ);
        h.retire(owner);
        check(h.canAccess(CLIENT, first, READ) && !h.canAccess(CLIENT, second, READ),
                "Only the selected URI actually persisted by the recipient outlives its owner");
        check(h.grants.list("android", false, true).size() == 1 && h.pm.get("android") == null,
                "A real provider registration needs no fabricated source APK");
        h.release(CLIENT, first, READ);

        Uri removed = Uri.parse("content://fixture.hosted.provider/selected/removed");
        IBinder liveOwner = token();
        publish.invoke(h.grants, liveOwner,
                List.of(prepare.invoke(h.grants, registration, CLIENT, removed, READ | PERSISTABLE)));
        var globalGrant = UriGrants.class.getDeclaredMethod("grantFromProvider", registrationType,
                String.class, Uri.class, int.class);
        globalGrant.setAccessible(true);
        globalGrant.invoke(h.grants, registration, OTHER, removed, READ | PERSISTABLE);
        globalGrant.invoke(h.grants, registration, CLIENT, second, READ);
        h.take(CLIENT, removed, READ); h.take(OTHER, removed, READ);
        var revoke = UriGrants.class.getDeclaredMethod("revokeFromProvider", registrationType,
                Uri.class, int.class);
        revoke.setAccessible(true);
        // DocumentsProvider.revokeDocumentPermission uses the complete mode mask.
        revoke.invoke(h.grants, registration, removed, ~0);
        check(!h.canAccess(CLIENT, removed, READ) && !h.canAccess(OTHER, removed, READ)
                && h.getPersisted(CLIENT, true).isEmpty() && h.getPersisted(OTHER, true).isEmpty(),
                "Provider revocation removes every recipient's owner, global and persisted modes");
        check(h.canAccess(CLIENT, second, READ), "Provider revocation preserves unrelated documents");
        h.retire(liveOwner);
    }

    private static void advancedPermissionQueries() throws Exception {
        Harness h = new Harness(null);
        Uri exact = Uri.parse("content://" + AUTHORITY + "/query/exact");
        h.grant(OWNER, CLIENT, exact, READ | PERSISTABLE);
        check(h.checkUri(CLIENT, exact, READ | PERSISTABLE) == PackageManager.PERMISSION_GRANTED,
                "DocumentsProvider's read/persistable query recognizes the offered permission");
        check(h.checkUri(CLIENT, exact, WRITE | PERSISTABLE) == PackageManager.PERMISSION_DENIED,
                "A persistable query still requires the requested access mode");
        check(h.checkUri(CLIENT, exact, READ | PERSISTABLE | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
                == PackageManager.PERMISSION_DENIED, "An exact record does not answer a prefix query");
        Uri plain = Uri.parse("content://" + AUTHORITY + "/query/plain");
        h.grant(OWNER, CLIENT, plain, READ);
        check(h.checkUri(CLIENT, plain, READ | PERSISTABLE) == PackageManager.PERMISSION_DENIED,
                "An ordinary global grant does not acquire persistable strength");
        Uri tree = Uri.parse("content://" + AUTHORITY + "/query/tree");
        h.grant(OWNER, CLIENT, tree, READ | PERSISTABLE | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        check(h.checkUri(CLIENT, Uri.withAppendedPath(tree, "child"),
                READ | PERSISTABLE | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) == PackageManager.PERMISSION_GRANTED,
                "An actual prefix offer answers its descendant's advanced query");
    }

    private static void disabledProviderPermissions() throws Exception {
        Harness h = new Harness(null);
        Uri exact = Uri.parse("content://" + AUTHORITY + "/disabled/exact");
        Uri tree = Uri.parse("content://" + AUTHORITY + "/disabled/tree");
        Uri child = Uri.withAppendedPath(tree, "child");
        Uri owned = Uri.parse("content://" + AUTHORITY + "/disabled/owned");
        h.grant(OWNER, CLIENT, exact, READ | PERSISTABLE);
        h.grant(OWNER, CLIENT, tree, READ | PERSISTABLE | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        IBinder owner = token();
        h.owned(owner, CLIENT, owned, READ);
        ParsedProviderImpl component = (ParsedProviderImpl)
                ((PackageImpl) h.records.get(OWNER).parsed).getProviders().get(0);
        component.setEnabled(false);

        // AOSP takes and queries the retained canonical record without resolving
        // an enabled provider; actual acquisition still needs an available one.
        h.take(CLIENT, exact, READ);
        check(h.getPersisted(CLIENT, true).size() == 1,
                "An existing offer can first be taken while its provider component is disabled");
        check(!h.canAccess(CLIENT, exact, READ),
                "Retained permission does not make a disabled provider available for access");
        h.take(CLIENT, tree, READ);
        check(h.getPersisted(CLIENT, true).size() == 2
                && h.checkUri(CLIENT, child, READ | PERSISTABLE
                        | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) == PackageManager.PERMISSION_GRANTED,
                "Disabled providers retain prefix offers and persisted permissions");
        check(h.checkUri(CLIENT, child, WRITE) == PackageManager.PERMISSION_DENIED,
                "A disabled provider does not expand retained read modes to write");
        h.rejected(SecurityException.class, () -> h.take(CLIENT, child, READ));
        check(h.checkUri(CLIENT, owned, READ) == PackageManager.PERMISSION_GRANTED,
                "A live owner grant remains queryable while the provider is disabled");
        h.retire(owner);
        check(h.checkUri(CLIENT, owned, READ) == PackageManager.PERMISSION_DENIED,
                "Owner retirement still removes transient permission with a disabled provider");
        check(h.checkUri(CLIENT, exact, READ | PERSISTABLE) == PackageManager.PERMISSION_GRANTED,
                "Owner retirement preserves an independently retained persisted permission");

        set(h.records.get(OWNER), "mEnabledOwner", "fixture-next-install:" + OWNER);
        check(h.checkUri(CLIENT, exact, READ) == PackageManager.PERMISSION_DENIED,
                "A retired source installation cannot answer a retained permission query");
        h.rejected(SecurityException.class, () -> h.take(CLIENT, exact, READ));
        set(h.records.get(OWNER), "mEnabledOwner", "fixture-install-owner:" + OWNER);
        set(h.records.get(CLIENT), "mEnabledOwner", "fixture-next-install:" + CLIENT);
        check(h.checkUri(CLIENT, exact, READ) == PackageManager.PERMISSION_DENIED,
                "A retired recipient installation cannot answer a retained permission query");
        h.rejected(SecurityException.class, () -> h.take(CLIENT, exact, READ));
        set(h.records.get(CLIENT), "mEnabledOwner", "fixture-install-owner:" + CLIENT);
        check(h.checkUri(CLIENT, exact, READ) == PackageManager.PERMISSION_GRANTED,
                "Only the original admitted installation owns the retained modes");

        h.ugm.clearGrantedUriPermissions(CLIENT, 0);
        check(h.getPersisted(CLIENT, true).isEmpty()
                && h.checkUri(CLIENT, exact, READ) == PackageManager.PERMISSION_DENIED,
                "Explicit administrative revocation remains effective while the provider is disabled");
        h.rejected(SecurityException.class, () -> h.take(CLIENT, exact, READ));
    }

    private static void persistenceDoesNotHoldStateLocks(File file, Uri uri) throws Exception {
        Harness h = new Harness(file);
        h.grant(OWNER, CLIENT, uri, READ | PERSISTABLE);
        h.take(CLIENT, uri, READ);
        Field lockField = UriGrants.class.getDeclaredField("mFileLock");
        lockField.setAccessible(true);
        Object fileLock = lockField.get(h.grants);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch queried = new CountDownLatch(1);
        Thread flush = new Thread(() -> {
            try { h.grants.flush(); } catch (Throwable error) { failure.set(error); }
        }, "fixture-uri-flush");
        flush.setDaemon(true);
        synchronized (fileLock) {
            flush.start();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            boolean blocked = false;
            while (!blocked && System.nanoTime() < deadline) {
                for (Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
                    if (!"uri-grants-persistence".equals(entry.getKey().getName())
                            || entry.getKey().getState() != Thread.State.BLOCKED) continue;
                    for (StackTraceElement frame : entry.getValue()) {
                        if (UriGrants.class.getName().equals(frame.getClassName())
                                && "writeState".equals(frame.getMethodName())) blocked = true;
                    }
                }
                if (!blocked) Thread.sleep(5);
            }
            check(blocked, "The real writer reached its file lock after taking a snapshot");
            Thread query = new Thread(() -> {
                try {
                    if (!h.canAccess(CLIENT, uri, READ)) throw new AssertionError("Concurrent grant lookup failed");
                    h.release(CLIENT, uri, READ);
                } catch (Throwable error) { failure.set(error); }
                finally { queried.countDown(); }
            }, "fixture-uri-query");
            query.setDaemon(true); query.start();
            check(queried.await(5, TimeUnit.SECONDS),
                    "A blocked file write does not hold the package or grant state monitor");
        }
        flush.join(5000);
        check(!flush.isAlive() && failure.get() == null, "Concurrent persistence completed: " + failure.get());
        // The release raced the older snapshot. A later queued write must win.
        h.grants.flush();
        Harness reloaded = new Harness(file);
        check(reloaded.getPersisted(CLIENT, true).isEmpty(),
                "A mutation during file IO is retained by the next durable snapshot");
    }

    public static void main(String[] args) throws Exception {
        contracts(true);
    }

    public static void contracts() throws Exception {
        contracts(true);
    }

    public static void contracts(boolean realBinder) throws Exception {
        binder = realBinder;
        // The Windows ServiceFixtureRunner initializes the real writable cache
        // region. A missing native library must fail this fixture explicitly.
        if (realBinder) System.loadLibrary("android_runtime");

        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        unsafe = (Unsafe) unsafeField.get(null);

        File stateFile = new File(System.getProperty("java.io.tmpdir"), "urigrants-f11-test-" + System.nanoTime() + ".xml");
        stateFile.deleteOnExit();

        Harness h = new Harness(stateFile);
        Uri uri1 = Uri.parse("content://" + AUTHORITY + "/item1");
        Uri uri2 = Uri.parse("content://" + AUTHORITY + "/item2");
        Uri uriTransient = Uri.parse("content://" + AUTHORITY + "/transient");
        Uri uriPersisted = Uri.parse("content://" + AUTHORITY + "/persisted");

        // 1. Test taking persistable grant without FLAG_GRANT_PERSISTABLE_URI_PERMISSION -> SecurityException.
        h.grant(OWNER, CLIENT, uri1, READ);
        check(h.canAccess(CLIENT, uri1, READ), "Client has transient read access to uri1");
        check(h.getPersisted(CLIENT, true).isEmpty(), "No persisted grants exist yet for CLIENT");

        h.rejected(SecurityException.class, () -> h.take(CLIENT, uri1, READ));
        check(h.getPersisted(CLIENT, true).isEmpty(), "Persisted grant was not created after rejected take");

        // 2. Test taking persistable grant with flag -> succeeds and appears in getPersistedUriPermissions.
        h.grant(OWNER, CLIENT, uri2, READ | PERSISTABLE);
        check(h.canAccess(CLIENT, uri2, READ), "Client has read access to uri2");
        check(h.getPersisted(CLIENT, true).isEmpty(), "Grant is not persisted before takePersistableUriPermission");

        // Validation checks on takePersistableUriPermission
        h.take(CLIENT, uri2, 0);
        check(h.getPersisted(CLIENT, true).isEmpty(), "Zero modes do not persist a grant");
        h.rejected(IllegalArgumentException.class, () -> h.take(CLIENT, uri2, PERSISTABLE));
        h.rejected(IllegalArgumentException.class, () -> h.take(CLIENT, uri2, READ | 0x40000000));
        h.rejected(SecurityException.class, () -> h.take(CLIENT, uri2, WRITE));
        h.rejected(SecurityException.class, () -> h.take(OTHER, uri2, READ));

        // Successful take
        h.take(CLIENT, uri2, READ);
        h.grants.flush();
        check(stateFile.isFile(), "Taking a permission commits its state file");
        check(!new File(stateFile.getPath() + ".new").exists(),
                "Successful persistence leaves no unfinished replacement");
        List<UriPermission> clientIncoming = h.getPersisted(CLIENT, true);
        check(clientIncoming.size() == 1, "Exactly one persisted grant found for CLIENT");
        UriPermission p1 = clientIncoming.get(0);
        check(uri2.equals(p1.getUri()), "Persisted permission URI matches");
        check(p1.isReadPermission(), "Persisted permission is read");
        check(!p1.isWritePermission(), "Persisted permission is not write");
        check(p1.getPersistedTime() != UriPermission.INVALID_TIME, "Persisted permission timestamp is valid");

        // Outgoing persisted permission from OWNER perspective
        List<UriPermission> ownerOutgoing = h.getPersisted(OWNER, false);
        check(ownerOutgoing.size() == 1, "Owner sees outgoing persisted grant");
        check(uri2.equals(ownerOutgoing.get(0).getUri()), "Owner outgoing URI matches");

        // 3. Test releasing persistable grant -> removed from list.
        h.release(CLIENT, uri2, READ);
        check(h.getPersisted(CLIENT, true).isEmpty(), "Persisted grant removed from list after release");
        check(h.getPersisted(OWNER, false).isEmpty(), "Persisted grant removed from owner outgoing list");

        // The transient grant still exists, so repeated/zero-mode releases are no-ops.
        h.release(CLIENT, uri2, READ);
        h.release(CLIENT, uri2, 0);
        check(h.canAccess(CLIENT, uri2, READ), "Release preserves the live transient grant");

        // Exact and prefix records for the same URI both participate in take/release.
        Harness variants = new Harness(null);
        variants.grant(OWNER, CLIENT, uri2, READ | PERSISTABLE);
        variants.grant(OWNER, CLIENT, uri2, READ | WRITE | PERSISTABLE
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        variants.take(CLIENT, uri2, READ);
        check(variants.getPersisted(CLIENT, true).size() == 2,
                "Take covers both valid exact and prefix entries for the same URI");
        variants.take(CLIENT, uri2, WRITE);
        variants.release(CLIENT, uri2, READ);
        List<UriPermission> remaining = variants.getPersisted(CLIENT, true);
        check(remaining.size() == 1 && remaining.get(0).isWritePermission()
                && !remaining.get(0).isReadPermission(), "Release updates both URI variants");

        // Administrative results describe recipients, including the all-packages query.
        Harness admin = new Harness(null);
        admin.grant(OWNER, CLIENT, uri2, READ | PERSISTABLE);
        admin.grant(OWNER, OTHER, uri2, READ | PERSISTABLE);
        admin.take(CLIENT, uri2, READ);
        admin.take(OTHER, uri2, READ);
        List<GrantedUriPermission> granted = admin.getGranted(CLIENT);
        check(granted.size() == 1 && CLIENT.equals(granted.get(0).packageName)
                && uri2.equals(granted.get(0).uri), "Administration returns recipient grant records");
        check(admin.getGranted(null).size() == 2, "Null administration filter lists all recipients");
        admin.ugm.clearGrantedUriPermissions(OWNER, 0);
        check(admin.getGranted(null).size() == 2, "Clearing a source does not erase its outgoing grants");
        admin.ugm.clearGrantedUriPermissions(CLIENT, 0);
        check(admin.getGranted(CLIENT).isEmpty() && admin.getGranted(OTHER).size() == 1,
                "Administrative clearing affects only the selected recipient");
        admin.ugm.clearGrantedUriPermissions(null, 0);
        check(admin.getGranted(null).isEmpty(), "Null clear filter clears recipients for user 0");

        Harness owned = new Harness(null);
        IBinder first = token(), second = token();
        owned.owned(first, CLIENT, uri2, READ | PERSISTABLE);
        owned.owned(second, CLIENT, uri2, WRITE | PERSISTABLE);
        owned.take(CLIENT, uri2, READ | WRITE);
        check(owned.getPersisted(CLIENT, true).size() == 1,
                "Multiple Intent owners contribute to one canonical permission");
        owned.retire(first); owned.retire(second);
        check(owned.canAccess(CLIENT, uri2, READ | WRITE),
                "Persisted access survives both originating owners");
        owned.release(CLIENT, uri2, READ | WRITE);
        check(!owned.canAccess(CLIENT, uri2, READ | WRITE),
                "Releasing the last durable modes retires an ownerless permission");

        Harness shared = new Harness(null);
        shared.grant(OWNER, CLIENT, uri2, READ | PERSISTABLE);
        shared.grant(OWNER, PEER, uri2, WRITE | PERSISTABLE);
        shared.take(PEER, uri2, READ | WRITE);
        List<UriPermission> sharedRecords = shared.getPersisted(CLIENT, true);
        check(sharedRecords.size() == 1 && sharedRecords.get(0).isReadPermission()
                && sharedRecords.get(0).isWritePermission(),
                "Shared-UID grants coalesce and a peer can persist the UID's modes");
        check(shared.getPersisted(PEER, true).isEmpty(),
                "The public incoming list retains the canonical target-package filter");
        shared.release(PEER, uri2, READ);
        check(!shared.getPersisted(CLIENT, true).get(0).isReadPermission(),
                "A shared-UID peer releases the canonical persisted mode");

        // 4. Test server restart (simulate reload from file) -> persistable grant restored.
        // 5. Test non-persistable transient grants are NOT restored across restart.
        h.grant(OWNER, CLIENT, uriTransient, READ);
        h.grant(OWNER, CLIENT, uriPersisted, READ | WRITE | PERSISTABLE);
        h.take(CLIENT, uriPersisted, READ | WRITE);

        check(h.canAccess(CLIENT, uriTransient, READ), "Client has access to transient URI before restart");
        check(h.canAccess(CLIENT, uriPersisted, READ | WRITE), "Client has access to persisted URI before restart");
        check(h.getPersisted(CLIENT, true).size() == 1, "Persisted grant registered before restart");

        // Exercise the grant-retirement method used by ActivityTaskManager force stop.
        // This fixture uses actual metadata publication, not process teardown or
        // signed APK installation (covered by the package lifecycle suite).
        h.clearTransient(CLIENT);
        check(!h.canAccess(CLIENT, uriTransient, READ), "Force-stop retirement drops transient access");
        check(h.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "Force-stop retirement preserves persisted access");
        h.replace(CLIENT); h.replace(OWNER);
        check(h.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "Durable access survives source and recipient metadata replacement");
        h.grants.flush();

        // A partial replacement left by an interrupted write must not replace the
        // last complete state when the server starts again.
        File interruptedWrite = new File(stateFile.getPath() + ".new");
        Files.writeString(interruptedWrite.toPath(), "<uri-grants><incomplete",
                StandardCharsets.UTF_8);
        // Simulate server shutdown and restart by creating a new Harness reusing the state file.
        Harness hRestarted = new Harness(stateFile);
        check(!interruptedWrite.exists(), "Reload discards an interrupted replacement");
        check(!hRestarted.canAccess(CLIENT, uriTransient, READ),
                "Non-persistable transient grant is NOT restored across restart");
        check(hRestarted.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "Persistable grant is restored into active grant table across restart");

        List<UriPermission> restoredList = hRestarted.getPersisted(CLIENT, true);
        check(restoredList.size() == 1, "Restored persisted grant list contains exactly 1 entry");
        UriPermission restoredPerm = restoredList.get(0);
        check(uriPersisted.equals(restoredPerm.getUri()), "Restored grant URI matches");
        check(restoredPerm.isReadPermission(), "Restored grant is read");
        check(restoredPerm.isWritePermission(), "Restored grant is write");
        check(restoredPerm.getPersistedTime() != UriPermission.INVALID_TIME, "Restored grant timestamp is valid");

        hRestarted.release(CLIENT, uriPersisted, READ);
        UriPermission partial = hRestarted.getPersisted(CLIENT, true).get(0);
        check(!partial.isReadPermission() && partial.isWritePermission(),
                "Partial release after reload clears only the persisted read mode");
        // Pinned AOSP checkUriPermissionLocked uses getStrength(), including the
        // retained persistable offer, while another active mode keeps this entry.
        check(hRestarted.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "The remaining entry retains AOSP persistable permission strength");
        hRestarted.take(CLIENT, uriPersisted, READ);
        check(hRestarted.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "A restored permission retains its persistable offer");

        // Releasing on the restarted server removes it from memory, list and disk
        hRestarted.release(CLIENT, uriPersisted, READ | WRITE);
        check(hRestarted.getPersisted(CLIENT, true).isEmpty(), "Restored grant removed after release");
        check(!hRestarted.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "Access is revoked after releasing restored persistable grant");
        hRestarted.grants.flush();

        // Second reload to verify release was persisted to disk
        Harness hSecondRestart = new Harness(stateFile);
        check(hSecondRestart.getPersisted(CLIENT, true).isEmpty(),
                "Released grant remains gone across subsequent restart");
        check(!hSecondRestart.canAccess(CLIENT, uriPersisted, READ | WRITE),
                "Access remains revoked across subsequent restart");

        // Android 16 retains at most 512 persisted GrantUri entries per recipient UID.
        Harness quota = new Harness(null);
        Uri oldest = Uri.parse("content://" + AUTHORITY + "/quota/0");
        for (int i = 0; i <= 512; i++) {
            Uri item = Uri.parse("content://" + AUTHORITY + "/quota/" + i);
            quota.grant(OWNER, CLIENT, item, READ | PERSISTABLE);
            quota.take(CLIENT, item, READ);
        }
        List<UriPermission> limited = quota.getPersisted(CLIENT, true);
        check(limited.size() == 512, "Persisted grants obey the Android 16 limit");
        check(limited.stream().noneMatch(permission -> oldest.equals(permission.getUri())),
                "Quota pruning releases the oldest persisted permission");
        check(quota.canAccess(CLIENT, oldest, READ), "Quota pruning preserves a live transient grant");

        File lifecycleFile = new File(stateFile.getParentFile(), "uri-lifecycle.xml");
        Harness lifecycle = new Harness(lifecycleFile);
        lifecycle.grant(OWNER, CLIENT, uriPersisted, READ | PERSISTABLE);
        lifecycle.take(CLIENT, uriPersisted, READ);
        lifecycle.grants.flush();
        ((PackageImpl)lifecycle.records.get(OWNER).parsed).setEnabled(false);
        lifecycle.take(CLIENT, uriPersisted, READ);
        check(lifecycle.checkUri(CLIENT, uriPersisted, READ) == PackageManager.PERMISSION_GRANTED,
                "A retained grant remains queryable after disabling its provider package");
        check(lifecycle.checkUri(CLIENT, uriPersisted, READ | PERSISTABLE)
                == PackageManager.PERMISSION_GRANTED,
                "Disabled provider packages preserve retained persistable strength");
        check(lifecycle.checkUri(CLIENT, uriPersisted, WRITE) == PackageManager.PERMISSION_DENIED,
                "A retained read grant does not gain write permission during disablement");
        check(!lifecycle.canAccess(CLIENT, uriPersisted, READ),
                "Grant queries do not bypass disabled-package acquisition checks");
        check(lifecycle.getPersisted(CLIENT, true).size() == 1,
                "Persisted listings retain records while their provider package is disabled");
        ((PackageImpl)lifecycle.records.get(OWNER).parsed).setEnabled(true);
        // A fresh installation gets a different durable package-owner epoch.
        // Read the real old snapshot after seeding the newly admitted metadata.
        Harness reinstalled = new Harness(null);
        set(reinstalled.records.get(CLIENT), "mEnabledOwner", "fixture-next-install:" + CLIENT);
        UriGrants afterInstall = new UriGrants(reinstalled.pm, reinstalled.atm, lifecycleFile);
        afterInstall.load();
        check(afterInstall.list(CLIENT, true, true).isEmpty(),
                "A recipient reinstall does not restore its previous installation's grants");
        Harness newSource = new Harness(null);
        set(newSource.records.get(OWNER), "mEnabledOwner", "fixture-next-install:" + OWNER);
        UriGrants afterSourceInstall = new UriGrants(newSource.pm, newSource.atm, lifecycleFile);
        afterSourceInstall.load();
        check(afterSourceInstall.list(CLIENT, true, true).isEmpty(),
                "A provider reinstall does not restore its previous installation's grants");
        lifecycle.removeCapabilities(OWNER);
        check(lifecycle.getPersisted(CLIENT, true).isEmpty()
                && !lifecycle.canAccess(CLIENT, uriPersisted, READ),
                "The package-removal callback retires durable and transient provider grants");
        lifecycle.grants.flush();
        check(new Harness(lifecycleFile).getPersisted(CLIENT, true).isEmpty(),
                "Package-removal cleanup is durable across restart");
        persistenceDoesNotHoldStateLocks(new File(stateFile.getParentFile(), "uri-concurrency.xml"), uriPersisted);
        registeredProviderBatch();
        advancedPermissionQueries();
        disabledProviderPermissions();

        stateFile.delete();
        System.out.println("PersistableUriGrantContractTest all passed: " + checks + " checks");
    }
}
