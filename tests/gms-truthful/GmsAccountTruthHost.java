/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.IAccountManagerResponse;
import android.app.PropertyInvalidatedCache;
import android.content.pm.Signature;
import android.os.Bundle;

import com.android.internal.pm.parsing.pkg.PackageImpl;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BiConsumer;
import java.util.function.LongSupplier;
import sun.misc.Unsafe;

/**
 * Issue #335, account and token path: no Google account or token exists unless an
 * authenticator provides it. Unchanged AccountManagerService and SyncManager; boundaries
 * are constructed installed-package metadata, the JNI identity/allocation host library,
 * the protected-store writer (Windows DPAPI) and a non-running sync executor.
 *
 * <p>Android contract (android-16.0.0_r4): AccountManager.getAccountsByType lists only
 * accounts an authenticator added; getAuthToken for a type without an authenticator
 * fails its Session bind ("bind failure", ERROR_CODE_REMOTE_EXCEPTION), which
 * AccountManager reports as AuthenticatorException.
 */
public final class GmsAccountTruthHost {
    static final String GMS = "com.google.android.gms", CLIENT = "fixture.gms.accounts.client";
    static final Account SYNTHETIC = new Account("user@gesher.local", "com.google");
    static final Account REAL = new Account("someone@example.org", "com.google");
    static final Unsafe U;
    static int checks;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); U = (Unsafe)f.get(null); }
        catch (ReflectiveOperationException e) { throw new ExceptionInInitializerError(e); }
    }
    static void check(boolean v, String why) { if (!v) throw new AssertionError(why); checks++; }
    static Class<?> t(String s) throws Exception { return Class.forName("com.gesher.server." + s); }
    static Field f(Class<?> c, String n) throws Exception { Field f = c.getDeclaredField(n); f.setAccessible(true); return f; }
    static Object get(Object o, String n) throws Exception { return f(o.getClass(), n).get(o); }
    static void set(Object o, String n, Object v) throws Exception { f(o.getClass(), n).set(o, v); }
    static Object make(Class<?> c, Class<?>[] p, Object... a) throws Exception {
        Constructor<?> k = c.getDeclaredConstructor(p); k.setAccessible(true); return k.newInstance(a);
    }
    static Object call(Object o, String n, Class<?>[] p, Object... a) throws Exception {
        Method m = o.getClass().getDeclaredMethod(n, p); m.setAccessible(true);
        try { return m.invoke(o, a); }
        catch (InvocationTargetException e) {
            if (e.getCause() instanceof Error z) throw z;
            if (e.getCause() instanceof Exception z) throw z;
            throw e;
        }
    }
    static Object pkg(String name, int uid) throws Exception {
        PackageImpl p = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        p.setEnabled(true); p.setTargetSdkVersion(36); p.hideAsFinal();
        Object a = U.allocateInstance(t("ApkPackage")); set(a, "parsed", p); set(a, "packageName", name); set(a, "mUid", uid);
        set(a, "apkPath", "/fixture/" + name + "/base.apk"); set(a, "mDataDir", "/fixture/" + name + "/data");
        set(a, "sharedLibraries", f(t("SharedLibraryGraph"), "EMPTY").get(null));
        set(a, "mSigningIdentityChecked", true); set(a, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{7})});
        return a;
    }

    /** The answer a client's AccountManagerFuture is built from. */
    static final class Response extends IAccountManagerResponse.Default {
        Bundle result; int code = -1; String message;
        @Override public void onResult(Bundle value) { result = value; }
        @Override public void onError(int errorCode, String errorMessage) { code = errorCode; message = errorMessage; }
    }

    /** The production services: package manager, accounts, and sync settings. */
    static final class H implements AutoCloseable {
        final Object pm, accounts; Properties durable;
        H(boolean microg, Properties stored) throws Exception {
            List<Object> installed = new ArrayList<>(List.of(pkg(CLIENT, 12002)));
            if (microg) installed.add(pkg(GMS, 12010));
            pm = make(t("PackageManagerService"), new Class<?>[]{Collection.class, boolean.class}, installed, false);
            Object policy = call(pm, "permissions", new Class<?>[0]); Object catalog = get(policy, "mCatalog");
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            accounts = make(t("AccountManagerService"), new Class<?>[]{t("ActivityManagerService"), t("PackageManagerService"), boolean.class},
                    null, pm, false);
            Class<?> writer = t("AccountManagerService$StoreWriter");
            set(accounts, "mStoreWriter", Proxy.newProxyInstance(writer.getClassLoader(), new Class<?>[]{writer}, (p, m, a) -> {
                durable = new Properties(); durable.putAll((Properties)a[0]); return null;
            }));
            // A persisted protected store, as load() authenticates and hands it on.
            if (stored != null) call(accounts, "loadProtectedState", new Class<?>[]{Properties.class}, stored);
        }
        Map<?, ?> entries() throws Exception { return (Map<?, ?>)get(accounts, "mAccounts"); }
        Account[] list(String type) throws Exception {
            return (Account[])call(accounts, "getAccountsAsUser", new Class<?>[]{String.class, int.class, String.class}, type, 0, null);
        }
        Account[] forClient(String type) throws Exception {
            return (Account[])call(accounts, "getAccountsByTypeForPackage", new Class<?>[]{String.class, String.class, String.class}, type, CLIENT, null);
        }
        Response token(Account account) throws Exception {
            Response r = new Response();
            try {
                call(accounts, "getAuthToken", new Class<?>[]{IAccountManagerResponse.class, Account.class, String.class,
                        boolean.class, boolean.class, Bundle.class}, r, account, "oauth2:profile", false, false, new Bundle());
            } catch (SecurityException denied) {
                r.code = -2; r.message = denied.getMessage();
            }
            // Whatever runs on the authenticator executor answers within this bound.
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
            while (r.result == null && r.code == -1 && System.nanoTime() < deadline) Thread.sleep(5);
            return r;
        }
        public void close() throws Exception {
            ((ExecutorService)get(accounts, "mCalls")).shutdownNow();
            ((ExecutorService)get(accounts, "mSessionTimeout")).shutdownNow();
            ((ExecutorService)get(pm, "mPackageEventExecutor")).shutdownNow();
        }
    }

    static boolean has(Account[] list, Account account) { return Arrays.asList(list).contains(account); }
    static String tokenOf(Response r) { return r.result == null ? null : r.result.getString(AccountManager.KEY_AUTHTOKEN); }

    /** A protected store with one account owned by microG's authenticator. */
    static Properties realStore(Object accounts) throws Exception {
        Properties p = new Properties();
        String identity = (String)call(accounts, "packageIdentity", new Class<?>[]{String.class}, GMS);
        p.setProperty("count", "1"); p.setProperty("pendingRemoval.count", "0"); p.setProperty("previousNameField", "1");
        p.setProperty("0.name", REAL.name); p.setProperty("0.type", REAL.type);
        p.setProperty("0.owner", GMS); p.setProperty("0.ownerIdentity", identity);
        p.setProperty("0.data.count", "0"); p.setProperty("0.token.count", "1");
        p.setProperty("0.token.0.type", "oauth2:profile"); p.setProperty("0.token.0.value", "microg-issued");
        p.setProperty("0.token.0.grant.count", "0"); p.setProperty("0.grantType.count", "0");
        p.setProperty("0.visibility.count", "0");
        return p;
    }

    /** Clean device, no microG: no Google account, and a token request finds no authenticator. */
    static void withoutMicrog() throws Exception {
        try (H h = new H(false, null)) {
            check(!h.entries().containsKey(SYNTHETIC), "an account the user never added exists");
            check(h.list("com.google").length == 0, "Google accounts listed: " + Arrays.toString(h.list("com.google")));
            check(h.list(null).length == 0, "accounts listed without an authenticator");
            check(h.forClient("com.google").length == 0, "an app sees a Google account");
            check(!(boolean)call(h.accounts, "canAccessAccount", new Class<?>[]{Account.class, String.class}, SYNTHETIC, CLIENT),
                    "an app may sync an account that does not exist");
            check(((Account[])call(h.accounts, "syncableAccounts", new Class<?>[0])).length == 0, "a syncable account exists");
            Response r = h.token(SYNTHETIC);
            check(tokenOf(r) == null, "a token without an authenticator: " + tokenOf(r));
            check(r.code == AccountManager.ERROR_CODE_REMOTE_EXCEPTION && "bind failure".equals(r.message),
                    "no authenticator is not Android's bind failure: code=" + r.code + " " + r.message);
            Response other = h.token(new Account("anyone@example.org", "com.google"));
            check(tokenOf(other) == null && other.code == AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                    "unknown Google account answered: code=" + other.code + " token=" + tokenOf(other));
        }
    }

    /** Upgrade: sync settings an app stored for the old synthetic account are removed on start. */
    static void upgrade() throws Exception {
        File settings = new File(System.getenv("ANDROID_DATA"), "system/sync.properties");
        settings.getParentFile().mkdirs();
        Properties persisted = new Properties();
        String synthetic = SYNTHETIC.type + "/" + SYNTHETIC.name, real = REAL.type + "/" + REAL.name;
        persisted.setProperty("auto." + synthetic + "/com.android.contacts", "true");
        persisted.setProperty("syncable." + synthetic + "/com.android.contacts", "1");
        persisted.setProperty("periodic." + synthetic + "/com.android.contacts|", "3600");
        persisted.setProperty("auto." + real + "/com.android.contacts", "true");
        persisted.setProperty("master", "true");
        try (FileOutputStream out = new FileOutputStream(settings)) { persisted.store(out, "upgrade fixture"); }
        try (H probe = new H(true, null); H h = new H(true, realStore(probe.accounts))) {
            ScheduledThreadPoolExecutor idle = new ScheduledThreadPoolExecutor(1) {
                @Override public void execute(Runnable task) { /* no sync runs in this host */ }
            };
            Object sync = make(t("SyncManager"), new Class<?>[]{t("ActivityManagerService"), t("PackageManagerService"),
                    LongSupplier.class, LongSupplier.class, ScheduledExecutorService.class},
                    null, h.pm, (LongSupplier)() -> 0L, (LongSupplier)() -> 0L, idle);
            call(h.accounts, "setSyncRetirement", new Class<?>[]{BiConsumer.class},
                    (BiConsumer<Set<Account>, Set<Account>>)(removed, retained) -> {
                        try { call(sync, "accountsRemoved", new Class<?>[]{Set.class, Set.class}, removed, retained); }
                        catch (Exception e) { throw new IllegalStateException(e); }
                    });
            Method retire;
            try { retire = h.accounts.getClass().getDeclaredMethod("retireSyntheticAccountResidue"); }
            catch (NoSuchMethodException missing) { throw new AssertionError("no start-up cleanup of the synthetic account's residue"); }
            retire.setAccessible(true); retire.invoke(h.accounts);
            Properties after = new Properties();
            try (FileInputStream in = new FileInputStream(settings)) { after.load(in); }
            for (String key : after.stringPropertyNames()) {
                check(!key.contains(SYNTHETIC.name), "synthetic account sync setting survived: " + key);
            }
            check("true".equals(after.getProperty("auto." + real + "/com.android.contacts")), "a real account's sync setting was lost");
            check("true".equals(after.getProperty("master")), "the master sync setting was lost");
            check(h.entries().containsKey(REAL) && !h.entries().containsKey(SYNTHETIC), "the account store changed");
            check(Arrays.equals(h.list("com.google"), new Account[]{REAL}), "accounts after upgrade: " + Arrays.toString(h.list("com.google")));
            idle.shutdownNow();
        }
    }

    /** Real microG installed: only the account its authenticator added, with its own token. */
    static void withMicrog() throws Exception {
        try (H probe = new H(true, null); H h = new H(true, realStore(probe.accounts))) {
            check(Arrays.equals(h.list("com.google"), new Account[]{REAL}), "accounts with microG: " + Arrays.toString(h.list("com.google")));
            check(!h.entries().containsKey(SYNTHETIC), "synthetic entry beside microG");
            check("microg-issued".equals(call(h.accounts, "peekAuthToken", new Class<?>[]{Account.class, String.class}, REAL, "oauth2:profile")),
                    "the token microG issued is not the stored one");
            Response r = h.token(SYNTHETIC);
            check(tokenOf(r) == null, "a token for an account microG never added: " + tokenOf(r));
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]); PropertyInvalidatedCache.setTestMode(true);
        withoutMicrog();
        upgrade();
        withMicrog();
        System.out.println("GMS account truth host passed: " + checks + " checks");
        System.out.println("Actual AccountManagerService and SyncManager; constructed packages, host JNI identity, protected-store writer. No authenticator APK or Binder transport.");
    }
}
