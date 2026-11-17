/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.ISyncStatusObserver;
import android.content.Intent;
import android.content.pm.Signature;
import android.os.Bundle;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.*;
import sun.misc.Unsafe;

/** Actual Installer/account/sync transactions with synthetic installed metadata.
 * Only account encryption is replaced by a declared serialization fixture seam;
 * DPAPI and cross-process Binder are not claimed by this contract. */
public final class AuthenticatorUninstallTest {
    private static final String OWNER = "fixture.authenticator.owner", OTHER = "fixture.authenticator.other";
    private static final Account RETIRED = new Account("one", "fixture.account"),
            RETAINED = new Account("two", "fixture.other");
    private static final String AUTHORITY = "fixture.rows";
    private static final Unsafe UNSAFE;
    public static int checks;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private static void check(boolean okay, String why) { if (!okay) throw new AssertionError(why); checks++; }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> type, String name) throws Exception { Field f = type.getDeclaredField(name); f.setAccessible(true); return f; }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Object make(Class<?> type, Class<?>[] signature, Object... values) throws Exception {
        Constructor<?> ctor = type.getDeclaredConstructor(signature); ctor.setAccessible(true); return ctor.newInstance(values);
    }
    private static Object call(Object target, String name, Class<?>[] signature, Object... values) throws Exception {
        Class<?> cls = target instanceof Class<?> c ? c : target.getClass();
        Method method = cls.getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(target instanceof Class<?> ? null : target, values); }
        catch (InvocationTargetException error) {
            if (error.getCause() instanceof Exception failure) throw failure;
            if (error.getCause() instanceof Error failure) throw failure;
            throw error;
        }
    }
    private interface Action { void run() throws Exception; }
    private static void fails(Class<?> expected, Action action, String why) throws Exception {
        try { action.run(); } catch (Exception failure) { check(expected.isInstance(failure), why + ": " + failure); return; }
        throw new AssertionError(why + ": did not fail");
    }
    private static Properties read(Path file) throws IOException {
        Properties p = new Properties(); try (var in = Files.newInputStream(file)) { p.load(in); } return p;
    }
    private static void write(Properties values, Path file) throws IOException {
        Files.createDirectories(file.getParent());
        Path temporary = file.resolveSibling(file.getFileName() + ".fixture-tmp");
        try (FileOutputStream out = new FileOutputStream(temporary.toFile())) { values.store(out, "Synthetic fixture serialization, no DPAPI"); out.getFD().sync(); }
        Files.move(temporary, file, StandardCopyOption.REPLACE_EXISTING);
    }
    private static Object record(Path root, String name, int generation) throws Exception {
        Path code = root.resolve("app/" + name + "-" + generation); Files.createDirectories(code);
        Files.writeString(code.resolve("base.apk"), "Synthetic metadata; never parsed as an APK");
        Files.createDirectories(root.resolve("data/" + name));
        Files.writeString(root.resolve("data/" + name + "/marker"), "old fixture data");
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, code.resolve("base.apk").toString());
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
        Object result = UNSAFE.allocateInstance(type("ApkPackage"));
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "apkPath", parsed.getBaseApkPath());
        set(result, "mDataDir", root.resolve("data/" + name).toString()); set(result, "mUid", name.equals(OWNER) ? 12001 : 12002);
        set(result, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(result, "mSigningIdentityChecked", true); set(result, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{7, 8, 9})});
        return result;
    }
    @SuppressWarnings("unchecked")
    private static final class Harness implements AutoCloseable {
        final Path root = Path.of(System.getenv("ANDROID_DATA")), accountFile = root.resolve("system/accounts.sealed"), syncFile = root.resolve("system/sync.properties");
        final Object owner, other, pm, atm, installer, accounts, sync;
        final Map<Account, Object> entries;
        final Properties settings;
        final ScheduledThreadPoolExecutor timer = new ScheduledThreadPoolExecutor(1, r -> { Thread t = new Thread(r, "uninstall-fixture"); t.setDaemon(true); return t; });
        final List<Runnable> events = new ArrayList<>();
        final AtomicInteger notifications = new AtomicInteger();
        final AtomicBoolean failStage = new AtomicBoolean();
        Harness() throws Exception {
            check(!Files.exists(accountFile) && !Files.exists(root.resolve("system/packages.list")), "Fixture requires a fresh isolated device profile");
            owner = record(root, OWNER, 1); other = record(root, OTHER, 1);
            pm = make(type("PackageManagerService"), new Class<?>[]{Collection.class}, List.of(owner, other));
            set(pm, "mPackageEventExecutor", (Executor) events::add);
            set(pm, "mPackageEventSender", (Consumer<Intent>) intent -> notifications.incrementAndGet());
            atm = make(type("ActivityTaskManagerService"), new Class<?>[]{type("PackageManagerService")}, pm);
            Map<String, Object> registry = new LinkedHashMap<>();
            for (Object pkg : List.of(owner, other)) {
                String name = (String) get(pkg, "packageName");
                registry.put(name, make(type("Installer$Entry"), new Class<?>[]{String.class, String.class, long.class, long.class},
                        name, new File((String) get(pkg, "apkPath")).getParent(), 1L, 1L));
            }
            installer = make(type("Installer"), new Class<?>[]{type("PackageManagerService"), type("ActivityTaskManagerService"), Map.class}, pm, atm, registry);
            call(installer, "writeRegistry", new Class<?>[0]);
            accounts = make(type("AccountManagerService"), new Class<?>[]{type("ActivityManagerService"), type("PackageManagerService"), boolean.class}, null, pm, true);
            entries = (Map<Account, Object>) get(accounts, "mAccounts"); entries.clear();
            Class<?> writer = type("AccountManagerService$StoreWriter");
            set(accounts, "mStoreWriter", Proxy.newProxyInstance(writer.getClassLoader(), new Class<?>[]{writer}, (proxy, method, args) -> { write((Properties) args[0], accountFile); return null; }));
            try {
                Class<?> stager = type("AccountManagerService$StoreStager");
                set(accounts, "mStoreStager", Proxy.newProxyInstance(stager.getClassLoader(), new Class<?>[]{stager}, (proxy, method, args) -> {
                    if (failStage.get()) throw new IOException("Synthetic staged-store failure");
                    write((Properties) args[0], ((File) args[1]).toPath()); return null;
                }));
            } catch (ClassNotFoundException baseline) { /* removal-api must reach the old uninstall behavior. */ }
            entries.put(RETIRED, entry(OWNER)); entries.put(RETAINED, entry(OTHER)); save();
            sync = make(type("SyncManager"), new Class<?>[]{type("ActivityManagerService"), type("PackageManagerService"), LongSupplier.class, LongSupplier.class, ScheduledExecutorService.class},
                    null, pm, (LongSupplier) System::currentTimeMillis, (LongSupplier) System::currentTimeMillis, timer);
            try { call(accounts, "attachSyncManager", new Class<?>[]{type("SyncManager")}, sync); }
            catch (NoSuchMethodException baseline) { }
            set(sync, "mAccounts", (Supplier<Account[]>) () -> { synchronized (accounts) { return entries.keySet().toArray(new Account[0]); } });
            settings = (Properties) get(sync, "mSettings");
            settings.setProperty("master", "false");
            for (Account account : List.of(RETIRED, RETAINED)) {
                settings.setProperty("auto." + endpoint(account), "false");
                settings.setProperty("syncable." + endpoint(account), "0");
                settings.setProperty("periodic." + endpoint(account) + "|0", "3600|");
            }
            call(sync, "save", new Class<?>[0]);
            call(sync, "addStatusChangeListener", new Class<?>[]{int.class, ISyncStatusObserver.class}, ContentResolver.SYNC_OBSERVER_TYPE_SETTINGS,
                    new ISyncStatusObserver.Stub() { @Override public void onStatusChanged(int which) {
                        check(!Thread.holdsLock(accounts) && !Thread.holdsLock(sync), "Removal callback held a service monitor"); notifications.incrementAndGet();
                    }});
        }
        Object entry(String name) throws Exception {
            Object entry = make(type("AccountManagerService$Entry"), new Class<?>[0]);
            set(entry, "owner", name); set(entry, "ownerIdentity", call(accounts, "packageIdentity", new Class<?>[]{String.class}, name));
            set(entry, "password", "synthetic-password-" + name);
            ((Map<String, String>) get(entry, "data")).put("data", "synthetic-data");
            ((Map<String, String>) get(entry, "tokens")).put("scope", "synthetic-token-" + name);
            ((Map<String, Set<String>>) get(entry, "tokenGrants")).put("scope", new HashSet<>(Set.of("unrelated-client\u0000synthetic-code")));
            ((Map<String, Integer>) get(entry, "visibility")).put("unrelated-client\u0000synthetic-code", AccountManager.VISIBILITY_VISIBLE);
            ((Map<String, Object>) get(entry, "customTokens")).put("unrelated-client\u0000synthetic-code\u0000scope",
                    make(type("AccountManagerService$CachedToken"), new Class<?>[]{String.class, long.class}, "synthetic-custom", Long.MAX_VALUE));
            return entry;
        }
        String endpoint(Account account) throws Exception { return (String) call(type("SyncManager"), "id", new Class<?>[]{Account.class, String.class}, account, AUTHORITY); }
        void save() throws Exception { synchronized (accounts) { call(accounts, "beginMutation", new Class<?>[0]); call(accounts, "save", new Class<?>[0]); } }
        void password(String password) throws Exception {
            synchronized (accounts) { call(accounts, "beginMutation", new Class<?>[0]); set(entries.get(RETAINED), "password", password); call(accounts, "save", new Class<?>[0]); }
        }
        void uninstall() throws Exception { check((Boolean) call(installer, "uninstall", new Class<?>[]{String.class}, OWNER), "Installed authenticator not removed"); }
        void afterRegistry(Action action) throws Exception {
            Object original = get(pm, "mRemovalCoordinator");
            Class<?> coordinator = type("PackageManagerService$RemovalCoordinator"), commit = type("PackageManagerService$RemovalCommit");
            set(pm, "mRemovalCoordinator", Proxy.newProxyInstance(coordinator.getClassLoader(), new Class<?>[]{coordinator}, (proxy, method, args) -> {
                Object callback = args[2];
                Object wrapped = Proxy.newProxyInstance(commit.getClassLoader(), new Class<?>[]{commit}, (p, m, a) -> { call(callback, "run", new Class<?>[0]); action.run(); return null; });
                return call(original, "commit", new Class<?>[]{type("ApkPackage"), type("PackageRemovalStorage"), commit}, args[0], args[1], wrapped);
            }));
        }
        void recover() throws Exception { call(installer, "recoverRemovals", new Class<?>[0]); }
        void committed() throws Exception {
            check(call(pm, "get", new Class<?>[]{String.class}, OWNER) == null, "Removed package remained installed");
            check(!entries.containsKey(RETIRED) && entries.containsKey(RETAINED), "Account deletion affected the wrong owner");
            check(!Files.exists(root.resolve("data/" + OWNER)) && Files.exists(root.resolve("data/" + OTHER + "/marker")), "Package data retirement was not isolated");
            check(settings.keySet().stream().noneMatch(k -> k.toString().contains(endpointUnchecked(RETIRED))), "Retired sync settings survived in memory");
            check(settings.containsKey("auto." + endpoint(RETAINED)), "Unrelated sync settings disappeared");
        }
        String endpointUnchecked(Account account) { try { return endpoint(account); } catch (Exception e) { throw new AssertionError(e); } }
        @Override public void close() throws Exception {
            timer.shutdownNow(); ((ExecutorService) get(accounts, "mCalls")).shutdownNow();
            ((ExecutorService) get(accounts, "mSessionTimeout")).shutdownNow();
            ((Collection<Object>) field(type("DeviceWork"), "sSources").get(null)).remove(get(sync, "mWorkSource"));
        }
    }

    private static void commit() throws Exception {
        try (Harness h = new Harness()) {
            Object retained = h.entries.get(RETAINED), oldGeneration = get(h.entries.get(RETIRED), "generation");
            Object sessions = get(h.accounts, "mSessions"); String code = (String) call(h.accounts, "packageIdentity", new Class<?>[]{String.class}, OWNER);
            Bundle handle = (Bundle) call(sessions, "issue", new Class<?>[]{String.class, int.class, String.class, String.class, String.class, Account.class, Bundle.class},
                    "unrelated-client\u0000synthetic-code", 12002, OWNER, code, RETIRED.type, RETIRED, new Bundle());
            Object auth = make(type("AccountManagerService$Authenticator"), new Class<?>[]{String.class, ComponentName.class, int.class, int.class, int.class, int.class, boolean.class},
                    RETIRED.type, new ComponentName(OWNER, OWNER + ".Authenticator"), 0, 0, 0, 0, false);
            Object answer = make(type("AccountManagerService$Answer"), new Class<?>[]{type("AccountManagerService"), type("AccountManagerService$Authenticator")}, h.accounts, auth);
            h.uninstall(); h.committed();
            check(((CountDownLatch) get(answer, "done")).getCount() == 0
                    && (Integer) get(answer, "code") == AccountManager.ERROR_CODE_CANCELED, "Pending authenticator callback was not canceled");
            check(h.entries.get(RETAINED) == retained, "Unrelated account identity changed during removal");
            check(read(h.accountFile).values().stream().noneMatch(v -> v.toString().contains(OWNER)), "Removed credentials remain in the serialized account snapshot");
            check(!read(h.syncFile).containsKey("auto." + h.endpoint(RETIRED)), "Removed sync settings remain on disk");
            fails(SecurityException.class, () -> call(sessions, "consume", new Class<?>[]{Bundle.class, String.class, int.class, Predicate.class},
                    handle, "unrelated-client\u0000synthetic-code", 12002, (Predicate<Object>) p -> true), "Owner continuation survived uninstall");
            Object replacement = record(h.root, OWNER, 2); call(h.pm, "add", new Class<?>[]{type("ApkPackage")}, replacement);
            check(code.equals(call(h.accounts, "packageIdentity", new Class<?>[]{String.class}, OWNER)), "Fixture reinstall must use the same verified signer");
            check(!h.entries.containsKey(RETIRED), "Same-signer reinstall revived an account");
            check(call(h.accounts, "validatedResult", new Class<?>[]{type("AccountManagerService$Authenticator"), Bundle.class, Account.class}, auth, new Bundle(), null) == null,
                    "Pre-uninstall authenticator result survived same-signer reinstall");
            Bundle late = new Bundle(); late.putString(AccountManager.KEY_AUTHTOKEN, "synthetic-late-token");
            call(answer, "onResult", new Class<?>[]{Bundle.class}, late);
            check(get(answer, "result") == null, "Late authenticator callback republished a retired token");
            Object fresh = make(type("AccountManagerService$Entry"), new Class<?>[0]); h.entries.put(RETIRED, fresh);
            check(get(fresh, "generation") != oldGeneration && ((Map<?, ?>) get(fresh, "tokens")).isEmpty() && get(fresh, "password") == null,
                    "Recreated account inherited removed credentials or generation");
            for (Runnable event : h.events) event.run(); check(h.notifications.get() >= 3, "Committed removal did not notify sync/package observers");
        }
    }

    private static void rollback(boolean stagedFailure) throws Exception {
        try (Harness h = new Harness()) {
            byte[] accounts = Files.readAllBytes(h.accountFile), sync = Files.readAllBytes(h.syncFile), registry = Files.readAllBytes(h.root.resolve("system/packages.list"));
            Object entry = h.entries.get(RETIRED); h.failStage.set(stagedFailure);
            Path blocked = h.root.resolve("system/packages.list.tmp"); if (!stagedFailure) Files.createDirectory(blocked);
            fails(IOException.class, h::uninstall, "Uncommitted storage failure must reject uninstall");
            check(h.entries.get(RETIRED) == entry, "Rejected removal changed account identity");
            check(Arrays.equals(accounts, Files.readAllBytes(h.accountFile)) && Arrays.equals(sync, Files.readAllBytes(h.syncFile)), "Rejected removal changed shared stores");
            check(Arrays.equals(registry, Files.readAllBytes(h.root.resolve("system/packages.list"))) && Files.exists(h.root.resolve("data/" + OWNER + "/marker")), "Rejected registry commit lost installed state");
            check(h.notifications.get() == 0 && h.events.isEmpty(), "Rejected removal notified committed deletion");
            check(call(h.pm, "get", new Class<?>[]{String.class}, OWNER) == h.owner && !(Boolean) call(h.pm, "isRemovalPending", new Class<?>[]{String.class}, OWNER), "Rollback did not restore package availability");
            if (!stagedFailure) Files.delete(blocked); h.failStage.set(false); h.recover(); h.uninstall(); h.committed();
        }
    }

    private static void publication() throws Exception {
        try (Harness h = new Harness()) {
            Path held = h.syncFile.resolveSibling("sync.properties.fixture-held");
            h.afterRegistry(() -> { Files.move(h.syncFile, held); Files.createDirectory(h.syncFile); });
            fails(IOException.class, h::uninstall, "Postcommit publication failure must be reported"); h.committed();
            check(read(h.accountFile).values().stream().noneMatch(v -> v.toString().contains(OWNER)), "First replacement did not publish before second failed");
            String before = (String) get(h.entries.get(RETAINED), "password");
            fails(IllegalStateException.class, () -> h.password("too-early"), "New account writes must not overtake pending replacement");
            check(before.equals(get(h.entries.get(RETAINED), "password")), "Blocked account write changed memory");
            fails(IllegalStateException.class, () -> call(h.sync, "requireRemovalPublication", new Class<?>[0]), "New sync writes must not overtake pending replacement");
            Files.delete(h.syncFile); Files.move(held, h.syncFile);
            h.password("newer-unrelated-password");
            call(h.sync, "save", new Class<?>[0]);
            byte[] newer = Files.readAllBytes(h.accountFile); h.recover();
            check(Arrays.equals(newer, Files.readAllBytes(h.accountFile)), "Recovery replayed an old snapshot over a newer account write");
            check(!read(h.syncFile).containsKey("auto." + h.endpoint(RETIRED)), "Recovery retained retired sync settings");
            h.recover(); check(Arrays.equals(newer, Files.readAllBytes(h.accountFile)), "Repeated recovery was not idempotent");
        }
    }

    private static void recovery() throws Exception {
        Path root = Path.of(System.getenv("ANDROID_DATA")); Object pkg = record(root, OWNER, 1);
        Path code = new File((String) get(pkg, "apkPath")).getParentFile().toPath();
        Class<?> storage = type("PackageRemovalStorage"), shared = type("PackageRemovalStorage$SharedStore");
        @SuppressWarnings({"unchecked", "rawtypes"}) Object accountKind = Enum.valueOf((Class) shared, "ACCOUNTS");
        Path target = root.resolve("system/accounts.sealed"); Files.createDirectories(target.getParent()); Files.writeString(target, "original");
        Object before = make(storage, new Class<?>[]{String.class, String.class}, OWNER, code.toString());
        Path staged = (Path) call(before, "replacementFile", new Class<?>[]{shared}, accountKind); Files.writeString(staged, "uncommitted");
        call(before, "stage", new Class<?>[0]); call(storage, "recover", new Class<?>[]{Map.class}, Map.of(OWNER, code.toString()));
        check(Files.readString(target).equals("original") && Files.exists(code.resolve("base.apk")), "Precommit crash recovery published replacement or lost code");
        Object after = make(storage, new Class<?>[]{String.class, String.class}, OWNER, code.toString());
        staged = (Path) call(after, "replacementFile", new Class<?>[]{shared}, accountKind); Files.writeString(staged, "committed");
        call(after, "stage", new Class<?>[0]); call(storage, "recover", new Class<?>[]{Map.class}, Map.of());
        check(Files.readString(target).equals("committed") && !Files.exists(code), "Postcommit crash recovery failed to publish replacement");
        Files.writeString(target, "newer"); call(storage, "recover", new Class<?>[]{Map.class}, Map.of());
        check(Files.readString(target).equals("newer"), "Recovered journal replayed its replacement");
    }

    private static void retirementFailure() throws Exception {
        try (Harness h = new Harness()) {
            Path ownerMarker = h.root.resolve("system/package-enabled-owners/" + OWNER);
            h.afterRegistry(() -> Files.createDirectory(ownerMarker));
            fails(IllegalStateException.class, h::uninstall, "Postcommit owner metadata failure must be reported");
            check(!h.entries.containsKey(RETIRED) && call(h.pm, "get", new Class<?>[]{String.class}, OWNER) == h.owner,
                    "Fixture did not reach committed account retirement and failed package publication");
            check((Boolean) call(h.pm, "isRemovalPending", new Class<?>[]{String.class}, OWNER), "Failed package publication released the retirement barrier");
            fails(IllegalStateException.class, h::recover, "Unrepaired metadata failure must keep recovery blocked");
            check((Boolean) call(h.pm, "isRemovalPending", new Class<?>[]{String.class}, OWNER), "Failed recovery released the retirement barrier");
            Files.delete(ownerMarker); h.recover(); h.committed();
            check(!(Boolean) call(h.pm, "isRemovalPending", new Class<?>[]{String.class}, OWNER), "Successful recovery retained the package barrier");
        }
    }

    private static void collision() throws Exception {
        try (Harness h = new Harness()) {
            Account left = new Account("b/c|x", "a"), right = new Account("c|x", "a/b");
            h.entries.put(left, h.entry(OWNER)); h.entries.put(right, h.entry(OTHER)); h.save();
            String old = "a/b/c|x/" + AUTHORITY;
            h.settings.setProperty("auto." + old, "false"); h.settings.setProperty("syncable." + old, "0");
            h.settings.setProperty("periodic." + old + "|0", "3600|"); call(h.sync, "save", new Class<?>[0]);
            h.uninstall();
            check(!h.entries.containsKey(left) && h.entries.containsKey(right), "Delimiter-containing accounts collided during deletion");
            for (String prefix : List.of("auto.", "syncable.", "periodic.")) {
                String suffix = prefix.equals("periodic.") ? "|0" : "";
                check(!h.settings.containsKey(prefix + h.endpoint(left) + suffix) && h.settings.containsKey(prefix + h.endpoint(right) + suffix), "Legacy sync-key migration lost or revived an account: " + prefix);
                check(!h.settings.containsKey(prefix + old + suffix), "Ambiguous legacy key survived migration");
            }
        }
    }

    public static void contracts(String group) throws Exception {
        switch (group) {
            case "removal-api" -> { try (Harness h = new Harness()) { h.uninstall(); h.committed(); } }
            case "commit" -> commit(); case "rollback" -> rollback(false); case "staging" -> rollback(true);
            case "publication" -> publication(); case "recovery" -> recovery(); case "collision" -> collision();
            case "retirement" -> retirementFailure();
            default -> throw new IllegalArgumentException("Unknown authenticator uninstall case: " + group);
        }
    }
    public static void main(String[] args) throws Exception {
        for (String group : args.length == 0 ? new String[]{"removal-api", "commit", "rollback", "staging", "publication", "recovery", "collision", "retirement"} : args) {
            android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("authenticator-uninstall-").toString(), true);
            contracts(group);
        }
        System.out.println("AuthenticatorUninstallTest all passed: " + checks + " checks; synthetic metadata and serialization, production uninstall/storage paths");
    }
}
