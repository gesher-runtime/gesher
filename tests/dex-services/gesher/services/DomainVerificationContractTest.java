/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.PackageManager;
import android.content.pm.verify.domain.DomainSet;
import android.content.pm.verify.domain.DomainVerificationInfo;
import android.content.pm.verify.domain.DomainVerificationManager;
import android.content.pm.verify.domain.IDomainVerificationManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceSpecificException;

import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import sun.misc.Unsafe;

/** Real public manager and production empty-record service; Windows also forces Binder parcels. */
public final class DomainVerificationContractTest {
    private static final String PRESENT = "android", ABSENT = "fixture.domain.not.installed";
    private static final UUID ID = UUID.fromString("64d09f1a-d10b-45d1-97c1-9d4a36de60c2");
    private static final Set<String> HOSTS = Set.of("example.com");
    private static int checks;

    private interface Call { void run() throws Exception; }
    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Call call, String name)
            throws Exception {
        try { call.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(name + ": " + failure, failure);
            checks++;
            return;
        }
        throw new AssertionError(name + ": no exception");
    }
    private static void unavailable(Call call, String name) throws Exception {
        try { call.run(); }
        catch (ServiceSpecificException failure) {
            check(failure.errorCode == DomainVerificationManager.INTERNAL_ERROR_NAME_NOT_FOUND, name);
            return;
        }
        throw new AssertionError(name + ": no absent-record error");
    }
    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(object, value);
    }
    private static PackageManagerService packages(boolean binder) throws Exception {
        if (binder) return new PackageManagerService(List.of());
        // The unrelated installer initializes native Binder state. Retain the real
        // PMS availability method and maps; this fixture does not model APK parsing.
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        PackageManagerService result = (PackageManagerService)
                ((Unsafe)field.get(null)).allocateInstance(PackageManagerService.class);
        set(result, "mPackages", Map.of());
        set(result, "mRemoving", Map.of());
        return result;
    }
    private static IDomainVerificationManager service(boolean binder) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.DeviceServices$DomainVerificationService");
        IDomainVerificationManager implementation;
        try {
            Constructor<?> constructor = type.getDeclaredConstructor(PackageManagerService.class);
            constructor.setAccessible(true);
            implementation = (IDomainVerificationManager)constructor.newInstance(packages(binder));
        } catch (NoSuchMethodException oldDefault) {
            // Permit the unchanged public fixture to identify the pre-fix behavior.
            Constructor<?> constructor = type.getDeclaredConstructor();
            constructor.setAccessible(true);
            implementation = (IDomainVerificationManager)constructor.newInstance();
        }
        if (!binder) return implementation;
        StandIn.setStrict(true);
        IBinder target = StandIn.of(IDomainVerificationManager.class, implementation).asBinder();
        check(target.getClass().getName().equals(
                "com.gesher.server.stub.IDomainVerificationManagerStub"), "generated domain Binder stub");
        IDomainVerificationManager proxy = IDomainVerificationManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        });
        check(proxy != implementation, "forced domain Binder proxy");
        return proxy;
    }
    private static DomainVerificationManager manager(IDomainVerificationManager service, int user) {
        Context context = new ContextWrapper(null) {
            @Override public int getUserId() { return user; }
        };
        return new DomainVerificationManager(context, service);
    }
    public static int run(boolean binder) throws Exception {
        checks = 0;
        IDomainVerificationManager service = service(binder);
        DomainVerificationManager manager = manager(service, 0);
        DomainSet domains = new DomainSet(HOSTS);

        // The real manager dereferences its Bundle result; Default's null fails here.
        check(manager.getUriRelativeFilterGroups(PRESENT, List.of("example.com")).isEmpty(),
                "public relative-filter query returns an empty map");
        check(manager.getUriRelativeFilterGroups(ABSENT, List.of("example.com")).isEmpty(),
                "absent records have no relative filters");
        check(manager.getUriRelativeFilterGroups(PRESENT, List.of()).isEmpty(), "empty domain query");
        Bundle first = service.getUriRelativeFilterGroups(PRESENT, List.of("example.com"));
        first.putString("example.com", "caller mutation");
        check(service.getUriRelativeFilterGroups(PRESENT, List.of("example.com")).isEmpty(),
                "relative-filter results do not publish caller mutations");
        List<String> names = manager.queryValidVerificationPackageNames();
        check(names != null && names.isEmpty(), "no valid verification records");
        names.add(PRESENT);
        check(manager.queryValidVerificationPackageNames().isEmpty(), "package-list snapshot isolation");
        for (String domain : List.of("example.com", "", "not a valid host")) {
            check(manager.getOwnersForDomain(domain).isEmpty(), "domain has no owners: " + domain);
        }
        check(manager.getDomainVerificationInfo(PRESENT) == null, "installed package has no record");
        check(manager.getDomainVerificationUserState(PRESENT) == null, "installed package has no user state");
        expect(PackageManager.NameNotFoundException.class,
                () -> manager.getDomainVerificationInfo(ABSENT), "absent info package");
        expect(PackageManager.NameNotFoundException.class,
                () -> manager.getDomainVerificationUserState(ABSENT), "absent user-state package");

        for (int state : new int[]{DomainVerificationInfo.STATE_SUCCESS,
                DomainVerificationInfo.STATE_FIRST_VERIFIER_DEFINED, Integer.MAX_VALUE}) {
            check(manager.setDomainVerificationStatus(ID, HOSTS, state)
                    == DomainVerificationManager.ERROR_DOMAIN_SET_ID_INVALID, "absent status ID");
        }
        for (boolean enabled : new boolean[]{true, false}) {
            check(manager.setDomainVerificationUserSelection(ID, HOSTS, enabled)
                    == DomainVerificationManager.ERROR_DOMAIN_SET_ID_INVALID, "absent selection ID");
            expect(PackageManager.NameNotFoundException.class,
                    () -> manager.setDomainVerificationLinkHandlingAllowed(PRESENT, enabled),
                    "absent link-handling record");
        }
        check(service.setDomainVerificationStatus(ID.toString(), new DomainSet(Set.of()),
                DomainVerificationInfo.STATE_SUCCESS) == DomainVerificationManager.ERROR_DOMAIN_SET_ID_INVALID,
                "absent raw ID takes precedence over empty-domain validation");
        for (String name : List.of(PRESENT, ABSENT)) {
            manager.setUriRelativeFilterGroups(name, Map.of());
            checks++;
            unavailable(() -> manager.setUriRelativeFilterGroups(name,
                    Map.of("example.com", List.of())), "filter update needs an attached record");
        }
        for (int state : new int[]{-1, 0, 2, 1023}) {
            expect(IllegalArgumentException.class,
                    () -> manager.setDomainVerificationStatus(ID, HOSTS, state), "invalid verifier state");
        }
        expect(IllegalArgumentException.class,
                () -> manager.setDomainVerificationStatus(ID, Set.of(), 1), "public empty domain set");
        expect(IllegalArgumentException.class,
                () -> manager.setDomainVerificationUserSelection(null, HOSTS, true), "public null ID");
        expect(IllegalArgumentException.class,
                () -> service.setDomainVerificationStatus("invalid-id", domains, 1), "raw malformed ID");
        expect(NullPointerException.class,
                () -> service.setDomainVerificationStatus(null, domains, 1), "raw null ID");
        expect(NullPointerException.class,
                () -> service.setDomainVerificationUserSelection(ID.toString(), null, true, 0),
                "raw null domain set");
        expect(NullPointerException.class, () -> manager.getOwnersForDomain(null), "null owner domain");
        expect(NullPointerException.class, () -> service.getOwnersForDomain(null, 0), "raw null owner domain");
        expect(NullPointerException.class, () -> manager.getDomainVerificationInfo(null), "null info package");
        expect(NullPointerException.class, () -> manager.getDomainVerificationUserState(null), "null user-state package");
        expect(NullPointerException.class,
                () -> service.getUriRelativeFilterGroups(null, List.of()), "raw null filter package");
        expect(NullPointerException.class,
                () -> service.getUriRelativeFilterGroups(PRESENT, null), "raw null filter domains");
        expect(NullPointerException.class,
                () -> service.setUriRelativeFilterGroups(PRESENT, null), "raw null filter groups");
        for (int user : new int[]{-1, 1}) {
            DomainVerificationManager other = manager(service, user);
            expect(IllegalArgumentException.class, () -> other.getOwnersForDomain("example.com"), "unknown owner user");
            expect(IllegalArgumentException.class, () -> other.getDomainVerificationUserState(PRESENT), "unknown query user");
            expect(IllegalArgumentException.class,
                    () -> other.setDomainVerificationUserSelection(ID, HOSTS, true), "unknown selection user");
            expect(IllegalArgumentException.class,
                    () -> other.setDomainVerificationLinkHandlingAllowed(PRESENT, true), "unknown link user");
        }
        check(manager.queryValidVerificationPackageNames().isEmpty()
                && manager.getOwnersForDomain("example.com").isEmpty()
                && manager.getUriRelativeFilterGroups(PRESENT, List.of("example.com")).isEmpty(),
                "failed updates never invent records or ownership");
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        try {
            System.out.println("DomainVerificationContractTest all passed: " + run(true) + " checks; binder=true");
        } finally {
            StandIn.setStrict(false);
        }
    }
}
