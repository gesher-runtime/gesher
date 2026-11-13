/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.IAccountManager;
import android.accounts.IAccountManagerResponse;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.ServiceManager;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Properties;

/**
 * Account security regressions. Default mode tests service policy/continuations
 * offline. remote-denied runs as an unattached non-system Binder client against
 * a real server. crypto-denied verifies the native bridge cannot seal app data
 * or fall back to plaintext from a non-SystemServer process.
 */
public final class AccountSecurityTest {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private interface Operation { void run() throws Exception; }

    private static void denied(Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { return; }
        catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) return;
            throw wrapped;
        }
        throw new AssertionError("Cross-account operation was accepted");
    }

    private static Method method(Class<?> type, String name, Class<?>... arguments) throws Exception {
        Method result = type.getDeclaredMethod(name, arguments);
        result.setAccessible(true); return result;
    }

    private static Object construct(Class<?> type, Class<?>[] arguments, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(arguments);
        constructor.setAccessible(true); return constructor.newInstance(values);
    }

    private static void field(Object target, String name, Object value) throws Exception {
        java.lang.reflect.Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true); field.set(target, value);
    }

    private static Object field(Object target, String name) throws Exception {
        java.lang.reflect.Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true); return field.get(target);
    }

    private static void policy() throws Exception {
        File temporary = Files.createTempDirectory("gesher-account-policy-").toFile();
        android.system.Os.setenv("ANDROID_DATA", temporary.getAbsolutePath(), true);
        Class<?> service = Class.forName("com.gesher.server.AccountManagerService");
        Class<?> pm = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
        Object packageManager = pm.getConstructor(java.util.Collection.class).newInstance(Collections.emptyList());
        Object manager = service.getConstructor(am, pm).newInstance(null, packageManager);
        Class<?> entry = Class.forName(service.getName() + "$Entry");
        Class<?> caller = Class.forName(service.getName() + "$Caller");
        Object record = construct(entry, new Class<?>[0]);
        field(record, "owner", "test.owner"); field(record, "ownerIdentity", "cert:original");
        Object owner = construct(caller, new Class<?>[] {String.class, String.class, boolean.class, int.class, int.class},
                "test.owner", "cert:original", false, 11000, 100);
        Object client = construct(caller, new Class<?>[] {String.class, String.class, boolean.class, int.class, int.class},
                "test.client", "cert:client", false, 11001, 101);
        Object replacement = construct(caller, new Class<?>[] {String.class, String.class, boolean.class, int.class, int.class},
                "test.owner", "cert:replacement", false, 11000, 102);
        method(service, "requireOwner", caller, entry).invoke(manager, owner, record);
        denied(() -> method(service, "requireOwner", caller, entry).invoke(manager, client, record));
        denied(() -> method(service, "requireOwner", caller, entry).invoke(manager, replacement, record));
        Account account = new Account("fixture@example.test", "test.type");
        Method visible = method(service, "visible", caller, Account.class, entry);
        require(!(Boolean) visible.invoke(manager, client, account, record), "An ungranted client saw an account");
        @SuppressWarnings("unchecked") Map<String, Integer> grants = (Map<String, Integer>) field(record, "visibility");
        String clientKey = (String) method(caller, "key").invoke(client);
        grants.put(clientKey, AccountManager.VISIBILITY_VISIBLE);
        require((Boolean) visible.invoke(manager, client, account, record), "Explicit client visibility was lost");
        Object clientReplacement = construct(caller, new Class<?>[] {String.class, String.class, boolean.class, int.class, int.class},
                "test.client", "cert:new", false, 11001, 103);
        require(!(Boolean) visible.invoke(manager, clientReplacement, account, record), "Visibility survived a client signer change");
        Bundle forged = new Bundle();
        forged.putInt(AccountManager.KEY_CALLER_UID, 1000);
        forged.putInt(AccountManager.KEY_CALLER_PID, 1);
        Bundle normalized = (Bundle) method(service, "options", caller, Bundle.class).invoke(manager, client, forged);
        require(normalized.getInt(AccountManager.KEY_CALLER_UID) == 11001
                && normalized.getInt(AccountManager.KEY_CALLER_PID) == 101, "Caller metadata was trusted");
        forged.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME, "test.owner");
        denied(() -> method(service, "options", caller, Bundle.class).invoke(manager, client, forged));
        System.out.println("AccountSecurityTest: owner/client/signer isolation and forged caller metadata denied");
        @SuppressWarnings("unchecked") Map<Account, Object> accounts = (Map<Account, Object>) field(manager, "mAccounts");
        accounts.put(account, record);
        grants.clear();
        Method accountVisibilities = method(service, "accountVisibilities", String.class, String.class, caller);
        @SuppressWarnings("unchecked") Map<String, Integer> hiddenNames = (Map<String, Integer>)
                accountVisibilities.invoke(manager, "test.client", account.type, client);
        require(!hiddenNames.containsKey(account.name), "Own-package visibility query leaked a hidden account name");
        grants.put(clientKey, AccountManager.VISIBILITY_VISIBLE);
        @SuppressWarnings("unchecked") Map<String, Integer> visibleNames = (Map<String, Integer>)
                accountVisibilities.invoke(manager, "test.client", account.type, client);
        require(visibleNames.get(account.name) == AccountManager.VISIBILITY_VISIBLE,
                "An explicitly granted own-package account was omitted");
        System.out.println("AccountSecurityTest: own-package visibility query does not leak hidden account names");
        Method tokenAuthorized = method(service, "tokenAuthorized", caller, entry, String.class);
        require(!(Boolean) tokenAuthorized.invoke(manager, client, record, "fixture"),
                "Account visibility silently authorized credentials");
        @SuppressWarnings("unchecked") Map<String, java.util.Set<String>> credentialGrants =
                (Map<String, java.util.Set<String>>) field(record, "tokenGrants");
        credentialGrants.put("fixture", new java.util.HashSet<>(java.util.Collections.singleton(clientKey)));
        require((Boolean) tokenAuthorized.invoke(manager, client, record, "fixture"), "Explicit credential grant was ignored");
        credentialGrants.get("fixture").remove(clientKey);
        require(!(Boolean) tokenAuthorized.invoke(manager, client, record, "fixture"),
                "Revoked credentials remained authorized through account visibility");
        System.out.println("AccountSecurityTest: visibility alone never authorizes normal tokens and revocation persists");

        field(record, "password", "old-fixture-password");
        @SuppressWarnings("unchecked") Map<String, String> oldTokens = (Map<String, String>) field(record, "tokens");
        oldTokens.put("fixture", "old-fixture-token");
        boolean failedStore = false;
        try { service.getMethod("setPassword", Account.class, String.class).invoke(manager, account, "new-fixture-password"); }
        catch (InvocationTargetException expected) { failedStore = expected.getCause() instanceof IllegalStateException; }
        require(failedStore, "Non-server test unexpectedly committed protected account changes");
        require("old-fixture-password".equals(service.getMethod("getPassword", Account.class).invoke(manager, account))
                && "old-fixture-token".equals(service.getMethod("peekAuthToken", Account.class, String.class)
                        .invoke(manager, account, "fixture")), "Failed protected write changed in-memory password or token state");
        require(!new File(temporary, "system/accounts.sealed").exists(), "Failed protected write created a data file");
        System.out.println("AccountSecurityTest: failed protected write rolls back password and token mutations");


        Class<?> auth = Class.forName(service.getName() + "$Authenticator");
        Object authenticator = construct(auth, new Class<?>[] {String.class, ComponentName.class,
                int.class, int.class, int.class, int.class, boolean.class},
                "test.type", new ComponentName("test.owner", "test.owner.Authenticator"), 0, 0, 0, 0, false);
        Method validate = method(service, "validatedResult", auth, Bundle.class, Account.class);
        Bundle result = new Bundle();
        result.putParcelable(AccountManager.KEY_INTENT, new Intent().setComponent(new ComponentName("test.owner", "test.owner.Login")));
        require(validate.invoke(manager, authenticator, result, account) != null, "An owned login Intent was rejected");
        result.putParcelable(AccountManager.KEY_INTENT, new Intent().setComponent(new ComponentName("test.other", "test.other.Login")));
        require(validate.invoke(manager, authenticator, result, account) == null, "External login Intent accepted");
        result.putParcelable(AccountManager.KEY_INTENT, new Intent().setComponent(new ComponentName("test.owner", "test.owner.Login"))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
        require(validate.invoke(manager, authenticator, result, account) == null, "Credential response granted a URI");
        Bundle token = new Bundle(); token.putString(AccountManager.KEY_AUTHTOKEN, "fake-token");
        token.putString(AccountManager.KEY_ACCOUNT_NAME, "wrong@example.test");
        token.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
        require(validate.invoke(manager, authenticator, token, account) == null, "Mismatched token account accepted");
        System.out.println("AccountSecurityTest: authenticator Intent and token-account confusion denied");

        Class<?> answer = Class.forName(service.getName() + "$Answer");
        Object session = construct(answer, new Class<?>[] {service, auth}, manager, authenticator);
        Bundle login = new Bundle();
        login.putParcelable(AccountManager.KEY_INTENT, new Intent().setComponent(new ComponentName("test.owner", "test.owner.Login")));
        method(answer, "onResult", Bundle.class).invoke(session, login);
        List<Bundle> received = new ArrayList<>();
        IAccountManagerResponse response = new IAccountManagerResponse.Stub() {
            @Override public void onResult(Bundle bundle) { received.add(bundle); }
            @Override public void onError(int code, String message) { throw new AssertionError("Unexpected session error " + code); }
        };
        Class<?> check = Class.forName(service.getName() + "$ResultCheck");
        Object identityCheck = Proxy.newProxyInstance(check.getClassLoader(), new Class<?>[] {check},
                (proxy, operation, arguments) -> arguments[0]);
        method(answer, "deliverSession", IAccountManagerResponse.class, check).invoke(session, response, identityCheck);
        Bundle finished = new Bundle(); finished.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
        method(answer, "onResult", Bundle.class).invoke(session, finished);
        method(answer, "onResult", Bundle.class).invoke(session, finished);
        require(received.size() == 2 && received.get(0).containsKey(AccountManager.KEY_INTENT)
                && received.get(1).getBoolean(AccountManager.KEY_BOOLEAN_RESULT), "Interactive continuation or final replay guard failed");
        System.out.println("AccountSecurityTest: login Activity continuation delivered once and final response replay ignored");
        Class<?> sessions = Class.forName("com.gesher.server.AccountSessions");
        long[] now = {1000};
        Object handles = construct(sessions, new Class<?>[] {java.util.function.LongSupplier.class},
                (java.util.function.LongSupplier) () -> now[0]);
        Method issue = method(sessions, "issue", String.class, int.class, String.class,
                String.class, String.class, Account.class, Bundle.class);
        Method consume = method(sessions, "consume", Bundle.class, String.class, int.class,
                java.util.function.Predicate.class);
        Bundle sensitive = new Bundle(); sensitive.putString(AccountManager.KEY_PASSWORD, "session-fixture-secret");
        Bundle envelope = (Bundle) issue.invoke(handles, "client-original", 11001, "test.owner",
                "cert:owner", "test.type", null, sensitive);
        require(envelope.size() == 1 && !envelope.containsKey(AccountManager.KEY_PASSWORD),
                "Session secrets were returned to a client");
        java.util.function.Predicate<Object> current = pending -> true;
        denied(() -> consume.invoke(handles, envelope, "other-client", 11002, current));
        denied(() -> consume.invoke(handles, envelope, "client-original", 11002, current));
        Bundle tampered = new Bundle(envelope); tampered.putString("password", "injected");
        denied(() -> consume.invoke(handles, tampered, "client-original", 11001, current));
        Object resumed = consume.invoke(handles, envelope, "client-original", 11001, current);
        Bundle privateData = (Bundle) method(resumed.getClass(), "data").invoke(resumed);
        require("session-fixture-secret".equals(privateData.getString(AccountManager.KEY_PASSWORD)),
                "Opaque session did not preserve authenticator data");
        denied(() -> consume.invoke(handles, envelope, "client-original", 11001, current));
        Bundle expires = (Bundle) issue.invoke(handles, "client-original", 11001, "test.owner",
                "cert:owner", "test.type", null, sensitive);
        now[0] += 600001;
        denied(() -> consume.invoke(handles, expires, "client-original", 11001, current));
        Bundle changed = (Bundle) issue.invoke(handles, "client-original", 11001, "test.owner",
                "cert:owner", "test.type", null, sensitive);
        denied(() -> consume.invoke(handles, changed, "client-original", 11001,
                (java.util.function.Predicate<Object>) pending -> false));
        Bundle restarted = (Bundle) issue.invoke(handles, "client-original", 11001, "test.owner",
                "cert:owner", "test.type", null, sensitive);
        Object freshHandles = construct(sessions, new Class<?>[0]);
        denied(() -> consume.invoke(freshHandles, restarted, "client-original", 11001, current));
        System.out.println("AccountSecurityTest: opaque sessions reject wrong clients, UID forgery, tampering, replay, expiry, owner replacement and restart");
        Class<?> sync = Class.forName("com.gesher.server.SyncManager");
        Object syncManager = construct(sync, new Class<?>[] {am, pm}, null, packageManager);
        Class<?> adapter = Class.forName(sync.getName() + "$Adapter");
        Object foreignAdapter = construct(adapter, new Class<?>[] {String.class, String.class,
                ComponentName.class, boolean.class, boolean.class, boolean.class, boolean.class, String.class},
                "test.authority", account.type, new ComponentName("test.foreign", "test.foreign.Sync"),
                true, true, true, false, null);
        method(sync, "enqueue", adapter, Account.class, Bundle.class)
                .invoke(syncManager, foreignAdapter, account, new Bundle());
        require(((List<?>) field(syncManager, "mPending")).isEmpty()
                && ((Map<?, ?>) field(syncManager, "mActive")).isEmpty(),
                "An unauthorized adapter received an account sync");
        System.out.println("AccountSecurityTest: unauthorized sync adapter cannot queue or receive an account");
        Files.delete(temporary.toPath());
    }

    private static void remoteDenied(String[] args) throws Exception {
        IAccountManager manager = IAccountManager.Stub.asInterface(ServiceManager.getService("account"));
        require(manager != null, "A real system server is required");
        Account account = new Account(args.length > 1 ? args[1] : "retained-account@example.test",
                args.length > 2 ? args[2] : "com.gesher.devsvc.account");
        denied(() -> manager.getPassword(account));
        denied(() -> manager.getUserData(account, "server"));
        denied(() -> manager.peekAuthToken(account, "persisted"));
        denied(() -> manager.setPassword(account, "attacker-password"));
        denied(() -> manager.setAuthToken(account, "persisted", "attacker-token"));
        denied(() -> manager.removeAccountExplicitly(account));
        denied(() -> manager.getAccountsAsUser(account.type, 0, "com.gesher.devsvc"));
        System.out.println("AccountSecurityTest: unattached Binder caller cannot read, modify, delete or impersonate account owner");
    }

    private static void authenticatorClient(String[] args) throws Exception {
        require(args.length == 3, "Pass metadata-only devsvc.apk and microgprobe.apk fixtures");
        android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File temporary = Files.createTempDirectory("gesher-authenticator-client-").toFile();
        android.system.Os.setenv("ANDROID_DATA", temporary.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        Object ownerApk = apk.getMethod("parse", String.class).invoke(null, args[1]);
        Object clientApk = apk.getMethod("parse", String.class).invoke(null, args[2]);
        String ownerName = (String) apk.getField("packageName").get(ownerApk);
        String clientName = (String) apk.getField("packageName").get(clientApk);
        Class<?> service = Class.forName("com.gesher.server.AccountManagerService");
        Class<?> pm = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
        Object packages = pm.getConstructor(java.util.Collection.class).newInstance(
                java.util.Arrays.asList(ownerApk, clientApk));
        Object manager = service.getConstructor(am, pm).newInstance(null, packages);
        Class<?> caller = Class.forName(service.getName() + "$Caller");
        Class<?> entry = Class.forName(service.getName() + "$Entry");
        Class<?>[] callerArguments = {String.class, String.class, boolean.class, int.class, int.class};
        String ownerCode = (String) method(service, "packageIdentity", String.class).invoke(manager, ownerName);
        String clientCode = (String) method(service, "packageIdentity", String.class).invoke(manager, clientName);
        require(ownerCode != null && clientCode != null, "Fixture code identities unavailable");
        int ownerUid = (int) method(apk, "uid").invoke(ownerApk);
        int clientUid = (int) method(apk, "uid").invoke(clientApk);
        Object owner = construct(caller, callerArguments, ownerName, ownerCode, false, ownerUid, 123);
        Object client = construct(caller, callerArguments, clientName, clientCode, false, clientUid, 124);
        Object staleOwner = construct(caller, callerArguments, ownerName, "stale-code", false, ownerUid, 123);
        Method query = method(service, "accountsByTypeForClient", String.class, String.class, caller);
        Method registered = method(service, "authenticator", String.class);
        String type = "com.gesher.devsvc.account";
        require(registered.invoke(manager, type) != null, "Real enabled fixture authenticator was not registered");
        @SuppressWarnings("unchecked") Map<Account, Object> accounts = (Map<Account, Object>) field(manager, "mAccounts");
        accounts.clear();
        Account visible = new Account("visible-delegated@example.test", type);
        Account hidden = new Account("hidden-delegated@example.test", type);
        String clientKey = (String) method(caller, "key").invoke(client);
        Object visibleEntry = null;
        for (Account account : java.util.Arrays.asList(visible, hidden,
                new Account("other-type@example.test", "unregistered.other"),
                new Account("foreign-owner@example.test", type),
                new Account("stale-owner@example.test", type))) {
            Object value = construct(entry, new Class<?>[0]);
            field(value, "owner", account.name.startsWith("foreign-") ? clientName : ownerName);
            field(value, "ownerIdentity", account.name.startsWith("foreign-") ? clientCode
                    : account.name.startsWith("stale-") ? "old-owner-code" : ownerCode);
            @SuppressWarnings("unchecked") Map<String, Integer> visibility = (Map<String, Integer>) field(value, "visibility");
            if (!account.equals(hidden)) visibility.put(clientKey, AccountManager.VISIBILITY_VISIBLE);
            accounts.put(account, value);
            if (account.equals(visible)) visibleEntry = value;
        }
        Account[] delegated = (Account[]) query.invoke(manager, type, clientName, owner);
        require(delegated.length == 1 && visible.equals(delegated[0]),
                "Typed owner delegation leaked hidden, foreign-owned, stale-code or other-type accounts");
        Account[] self = (Account[]) query.invoke(manager, type, ownerName, owner);
        require(self.length == 2 && java.util.Arrays.asList(self).contains(hidden),
                "Normal authenticator self enumeration changed");
        denied(() -> query.invoke(manager, type, ownerName, client));
        denied(() -> query.invoke(manager, null, clientName, owner));
        denied(() -> query.invoke(manager, "unregistered.other", clientName, owner));
        denied(() -> query.invoke(manager, type, clientName, staleOwner));
        denied(() -> query.invoke(manager, type, "missing.client", owner));
        require(!(Boolean) method(service, "tokenAuthorized", caller, entry, String.class)
                .invoke(manager, client, visibleEntry, "fixture-scope"),
                "Owner's enumeration granted a client token right");
        @SuppressWarnings("unchecked") Map<String, Integer> visibility = (Map<String, Integer>) field(visibleEntry, "visibility");
        require(visibility.size() == 1 && visibility.get(clientKey) == AccountManager.VISIBILITY_VISIBLE,
                "Owner enumeration mutated visibility");
        visibility.clear(); visibility.put(clientName + "\u0000old-client-code", AccountManager.VISIBILITY_VISIBLE);
        require(((Account[]) query.invoke(manager, type, clientName, owner)).length == 0,
                "Client code replacement inherited prior visibility");
        visibility.clear(); visibility.put(clientKey, AccountManager.VISIBILITY_VISIBLE);

        Object rawOwner = apk.getField("parsed").get(ownerApk);
        Object authenticatorService = null;
        for (Object component : (List<?>) rawOwner.getClass().getMethod("getServices").invoke(rawOwner)) {
            if (((String) component.getClass().getMethod("getName").invoke(component)).endsWith(".TestAuthenticator")) {
                authenticatorService = component; break;
            }
        }
        require(authenticatorService != null, "Cannot locate the real fixture authenticator component");
        Method enabled = authenticatorService.getClass().getMethod("setEnabled", boolean.class);
        enabled.invoke(authenticatorService, false);
        require(registered.invoke(manager, type) == null, "Disabled fixture remained registered");
        denied(() -> query.invoke(manager, type, clientName, owner));
        enabled.invoke(authenticatorService, true);
        rawOwner.getClass().getMethod("setEnabled", boolean.class).invoke(rawOwner, false);
        denied(() -> query.invoke(manager, type, clientName, owner));
        System.out.println("AccountSecurityTest: typed authenticator delegation preserves client visibility, owner/code/type isolation and credential denial; disabled or stale owners cannot delegate");
    }

    private static void cryptoDenied() throws Exception {
        boolean libraryPresent = false;
        for (String entry : System.getProperty("java.boot.class.path", "").split(
                java.util.regex.Pattern.quote(File.pathSeparator))) {
            if (entry.replace('\\', '/').endsWith("/gesher-app.jar")) {
                libraryPresent = new File(new File(entry).getParentFile(), "libgesher_account_crypto.so").isFile();
            }
        }
        require(libraryPresent, "The crypto denial test requires the actual native library");
        File temporary = Files.createTempDirectory("gesher-account-denial-").toFile();
        File destination = new File(temporary, "accounts.sealed");
        Class<?> store = Class.forName("com.gesher.server.AccountSecretStore");
        // A missing/unloadable bridge must fail this test, not masquerade as a
        // successful process-role rejection through write()'s IOException fallback.
        method(store, "ensureLoaded").invoke(null);
        byte[] payload = {1, 2, 3};
        denied(() -> method(store, "nativeProtect", byte[].class).invoke(null, payload));
        denied(() -> method(store, "nativeUnprotect", byte[].class).invoke(null, payload));
        Object instance = construct(store, new Class<?>[] {File.class}, destination);
        Properties values = new Properties(); values.setProperty("fake-secret", "non-personal-fixture");
        try {
            method(store, "write", Properties.class).invoke(instance, values);
            throw new AssertionError("A non-system-server process could use the account crypto bridge");
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof SecurityException,
                    "Unexpected crypto failure type");
        }
        require(!destination.exists(), "Failed crypto wrote plaintext account data");
        Files.delete(temporary.toPath());
        System.out.println("AccountSecurityTest: loaded native protect/unprotect reject non-server role without plaintext fallback");
    }

    private static void standinStorage() throws Exception {
        String previous = System.getenv("ANDROID_DATA");
        File temporary = Files.createTempDirectory("gesher-standin-accounts-").toFile();
        File system = new File(temporary, "system");
        require(system.mkdir(), "Cannot create fixture system directory");
        File sealed = new File(system, "accounts.sealed"), legacy = new File(system, "accounts");
        byte[] protectedMarker = {71, 65, 67, 50, 7, 11, 13};
        byte[] legacyMarker = "count=1\nfixture-only=original\n".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        Files.write(sealed.toPath(), protectedMarker);
        Files.write(legacy.toPath(), legacyMarker);
        try {
            android.system.Os.setenv("ANDROID_DATA", temporary.getAbsolutePath(), true);
            Class<?> service = Class.forName("com.gesher.server.AccountManagerService");
            Class<?> pm = Class.forName("com.gesher.server.PackageManagerService");
            Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
            Object packages = pm.getConstructor(java.util.Collection.class).newInstance(Collections.emptyList());
            Object manager = construct(service, new Class<?>[] {am, pm, boolean.class}, null, packages, false);
            @SuppressWarnings("unchecked") Map<Account, Object> accounts = (Map<Account, Object>)field(manager, "mAccounts");
            require(accounts.size() == 1, "Stand-in loaded persisted accounts instead of the synthetic fallback");
            method(service, "beginMutation").invoke(manager);
            method(service, "save").invoke(manager);
            require(java.util.Arrays.equals(Files.readAllBytes(sealed.toPath()), protectedMarker),
                    "Stand-in overwrote the device's protected account record");
            require(java.util.Arrays.equals(Files.readAllBytes(legacy.toPath()), legacyMarker),
                    "Stand-in imported or changed legacy device accounts");
            try {
                service.getConstructor(am, pm).newInstance(null, packages);
                throw new AssertionError("Persistent service accepted an invalid protected account record");
            } catch (InvocationTargetException expected) {
                require(expected.getCause() instanceof IllegalStateException, "Unexpected persistent-store rejection");
            }
        } finally {
            if (previous == null) android.system.Os.unsetenv("ANDROID_DATA");
            else android.system.Os.setenv("ANDROID_DATA", previous, true);
            Files.delete(sealed.toPath()); Files.delete(legacy.toPath());
            Files.delete(system.toPath()); Files.delete(temporary.toPath());
        }
        System.out.println("AccountSecurityTest: stand-in accounts ignore and preserve device records; persistent service remains fail-closed");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        if (args.length > 0 && args[0].equals("grant-probe")) {
            android.content.pm.IPackageManager packages = android.content.pm.IPackageManager.Stub.asInterface(
                    ServiceManager.getService("package"));
            IAccountManager accounts = IAccountManager.Stub.asInterface(ServiceManager.getService("account"));
            int uid = packages.getApplicationInfo("com.gesher.microgprobe", 0, 0).uid;
            String scope = args.length > 2 ? args[2] : "probe-ungranted";
            accounts.updateAppPermission(new Account("visible-ungranted@example.test", "com.gesher.devsvc.account"),
                    scope, uid, args.length > 1 && Boolean.parseBoolean(args[1]));
            System.out.println("AccountSecurityTest: explicit host fixture credential grant updated");
        }
        else if (args.length > 0 && args[0].equals("authenticator-client")) authenticatorClient(args);
        else if (args.length > 0 && args[0].equals("remote-denied")) remoteDenied(args);
        else if (args.length > 0 && args[0].equals("crypto-denied")) cryptoDenied();
        else if (args.length > 0 && args[0].equals("standin-storage")) standinStorage();
        else policy();
        System.out.println("AccountSecurityTest all passed");
    }
}
