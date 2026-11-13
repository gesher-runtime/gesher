/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.IApplicationThread;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/** Real account policy and process/activity bookkeeping, with synthetic identities. */
public final class GmsSetupContractTest {
    public static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Object target, String name) throws Exception {
        Field f = target.getClass().getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object target, String name) throws Exception {
        return field(target, name).get(target);
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target, name).set(target, value);
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args); m.setAccessible(true); return m;
    }
    private static Object make(Class<?> type, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(types); c.setAccessible(true); return c.newInstance(args);
    }
    @SuppressWarnings("unchecked")
    private static void accounts(Object pm) throws Exception {
        Class<?> service = Class.forName("com.gesher.server.AccountManagerService");
        Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
        Object manager = make(service, new Class<?>[] {am, pm.getClass(), boolean.class}, null, pm, false);
        Class<?> entryType = Class.forName(service.getName() + "$Entry");
        Object entry = make(entryType, new Class<?>[0]);
        set(entry, "owner", "fixture.owner"); set(entry, "ownerIdentity", "cert:fixture");
        Account account = new Account("fixture@example.test", "fixture.type");
        Map<Account, Object> records = (Map<Account, Object>) get(manager, "mAccounts");
        records.clear(); records.put(account, entry);
        Method read = service.getMethod("getAccountVisibility", Account.class, String.class);
        Method write = service.getMethod("setAccountVisibility", Account.class, String.class, int.class);
        String legacyVisible = AccountManager.PACKAGE_NAME_KEY_LEGACY_VISIBLE;
        String legacyHidden = AccountManager.PACKAGE_NAME_KEY_LEGACY_NOT_VISIBLE;
        check((int) read.invoke(manager, account, legacyVisible) == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE,
                "Missing visible-category default");
        check((int) read.invoke(manager, account, legacyHidden) == AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE,
                "Missing hidden-category default");
        check((boolean) write.invoke(manager, account, legacyVisible, AccountManager.VISIBILITY_VISIBLE),
                "Reserved visible key rejected");
        check((boolean) write.invoke(manager, account, legacyHidden, AccountManager.VISIBILITY_NOT_VISIBLE),
                "Reserved hidden key rejected");
        check((int) read.invoke(manager, account, legacyVisible) == AccountManager.VISIBILITY_VISIBLE,
                "Visible-category read lost its value");
        check((int) read.invoke(manager, account, legacyHidden) == AccountManager.VISIBILITY_NOT_VISIBLE,
                "Hidden-category read used a different key");
        Map<String, Integer> exported = (Map<String, Integer>) service
                .getMethod("getPackagesAndVisibilityForAccount", Account.class).invoke(manager, account);
        check(exported.get(legacyVisible) == AccountManager.VISIBILITY_VISIBLE
                        && exported.get(legacyHidden) == AccountManager.VISIBILITY_NOT_VISIBLE,
                "Owner map omitted reserved policy keys");
        Class<?> callerType = Class.forName(service.getName() + "$Caller");
        Object client = make(callerType, new Class<?>[] {String.class, String.class, boolean.class, int.class, int.class},
                "fixture.client", "cert:client", false, 11001, 123);
        Method visibility = method(service, "visibility", callerType, entryType);
        check((int) visibility.invoke(manager, client, entry) == AccountManager.VISIBILITY_NOT_VISIBLE,
                "Policy-category metadata changed an ordinary client's grant");
        Map<String, Integer> grants = (Map<String, Integer>) get(entry, "visibility");
        grants.put("fixture.client", AccountManager.VISIBILITY_VISIBLE);
        check((int) visibility.invoke(manager, client, entry) == AccountManager.VISIBILITY_NOT_VISIBLE,
                "Unscoped package-name entry changed visibility");
        String clientKey = (String) method(callerType, "key").invoke(client);
        grants.put(clientKey, AccountManager.VISIBILITY_VISIBLE);
        check((int) visibility.invoke(manager, client, entry) == AccountManager.VISIBILITY_VISIBLE,
                "Explicit client-code grant was lost");
        write.invoke(manager, account, legacyVisible, AccountManager.VISIBILITY_UNDEFINED);
        write.invoke(manager, account, legacyHidden, AccountManager.VISIBILITY_UNDEFINED);
        check((int) read.invoke(manager, account, legacyVisible) == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE
                        && (int) read.invoke(manager, account, legacyHidden) == AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE,
                "Undefined category did not restore its own default");
        Class<?> writer = Class.forName(service.getName() + "$StoreWriter");
        set(manager, "mStoreWriter", Proxy.newProxyInstance(writer.getClassLoader(), new Class<?>[] {writer},
                (p, m, a) -> { throw new java.io.IOException("fixture persistence failure"); }));
        boolean rejected = false;
        try { write.invoke(manager, account, legacyVisible, AccountManager.VISIBILITY_VISIBLE); }
        catch (InvocationTargetException failure) { rejected = failure.getCause() instanceof IllegalStateException; }
        check(rejected, "Failed policy persistence reported success");
        check((int) read.invoke(manager, account, legacyVisible) == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE,
                "Failed policy write was not rolled back");
    }

    @SuppressWarnings("unchecked")
    private static void idle(Object pm) throws Exception {
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object atm = make(atmType, new Class<?>[] {pm.getClass()}, pm);
        Class<?> processType = Class.forName(atmType.getName() + "$ProcessRecord");
        Object gms = make(processType, new Class<?>[] {atmType, int.class, IApplicationThread.class},
                atm, 123, new IApplicationThread.Default());
        String gmsPackage = "com.google.android.gms";
        set(gms, "packageName", gmsPackage); set(gms, "processName", gmsPackage);
        List<Object> processes = (List<Object>) get(atm, "mProcesses"); processes.add(gms);
        Method idleSince = method(atmType, "idleSince");
        check((long) idleSince.invoke(atm) == 0, "Binding GMS process counted as idle");
        AtomicInteger dispatched = new AtomicInteger();
        method(atmType, "withProcess", String.class, Consumer.class).invoke(atm, gmsPackage,
                (Consumer<Object>) ignored -> dispatched.incrementAndGet());
        check((long) idleSince.invoke(atm) == 0 && dispatched.get() == 0, "Pending bind work counted as idle");
        method(atmType, "finishAttachApplication", int.class).invoke(atm, 123);
        check(dispatched.get() == 1, "Ready GMS process did not receive queued work");
        check((long) idleSince.invoke(atm) > 0, "Ready background GMS prevented idle shutdown");
        ActivityInfo info = new ActivityInfo(); info.packageName = gmsPackage; info.name = "fixture.Settings";
        Class<?> activityType = Class.forName(atmType.getName() + "$ActivityRecord");
        Object activity = make(activityType,
                new Class<?>[] {atmType, ActivityInfo.class, Intent.class, int.class, int.class,
                        String.class, activityType, String.class, int.class},
                atm, info, new Intent(), 1, 123, gmsPackage, null, null, -1);
        List<Object> activities = (List<Object>) get(atm, "mActivities"); activities.add(activity);
        method(atmType, "attach", activityType, processType).invoke(atm, activity, gms);
        check((long) get(atm, "mIdleSince") == 0, "Opening GMS activity did not reset the timer");
        check((long) idleSince.invoke(atm) == 0, "Open GMS activity counted as idle");
        method(atmType, "remove", activityType).invoke(atm, activity);
        check((long) get(atm, "mIdleSince") > 0, "Closing GMS activity did not restart the timer");
        long since = (long) idleSince.invoke(atm);
        check((long) idleSince.invoke(atm) == since, "Polling restarted an already-idle timer");
        Object user = make(processType, new Class<?>[] {atmType, int.class, IApplicationThread.class},
                atm, 124, new IApplicationThread.Default());
        set(user, "packageName", "fixture.app"); set(user, "ready", true); processes.add(user);
        check((long) idleSince.invoke(atm) == 0, "Ordinary app process counted as idle");
        processes.remove(user);
        check((long) idleSince.invoke(atm) > 0, "Background-only transition stayed busy");
        activities.add(activity);
        check((long) idleSince.invoke(atm) == 0, "Unattached activity launch counted as idle");
        activities.clear();
        set(gms, "ready", false);
        method(atmType, "withProcess", String.class, Consumer.class).invoke(atm, gmsPackage,
                (Consumer<Object>) ignored -> dispatched.incrementAndGet());
        check((long) idleSince.invoke(atm) == 0, "New bind work counted as idle");
        // processDied removes the registered process; queued callbacks can outlive it.
        processes.remove(gms);
        check((long) idleSince.invoke(atm) > 0,
                "Orphaned queued work without a live or starting process prevented shutdown");
    }

    public static void contracts() throws Exception {
        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Object pm = make(pmType, new Class<?>[] {Collection.class}, Collections.emptyList());
        accounts(pm); idle(pm);
        System.out.println("GmsSetupContractTest all passed: " + checks);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        contracts();
    }
}
