/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.ResultInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PermissionInfo;
import android.content.pm.ActivityInfo;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.IBinder;
import android.os.Binder;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.BooleanSupplier;
import java.util.function.IntConsumer;
import sun.misc.Unsafe;

/**
 * Real request/ATM Java with constructed manifests and permission metadata,
 * controlled scheduling/clock and an isolated server-local radio. Nonpublic
 * access is reflective: the dex fixture and boot services have different loaders.
 */
public final class BluetoothRequestFixture {
    private static final String ENABLE = "android.bluetooth.adapter.action.REQUEST_ENABLE";
    private static final String DISABLE = "android.bluetooth.adapter.action.REQUEST_DISABLE";
    private static final String DISCOVERABLE = "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE";
    private static final String CONNECT = "android.permission.BLUETOOTH_CONNECT";
    private static final String ADVERTISE = "android.permission.BLUETOOTH_ADVERTISE";
    private static final String LEGACY = "android.permission.BLUETOOTH";
    private static final Unsafe UNSAFE;
    private static int checks;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true); UNSAFE = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Operation { void run() throws Exception; }
    private static void check(boolean okay, String message) {
        if (!okay) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Operation action, String message) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(message + ": " + failure, failure);
            checks++; return;
        }
        throw new AssertionError(message + ": no exception");
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> owner, String name) throws Exception {
        Field field = owner.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    private static Object construct(Class<?> owner, Class<?>[] signature, Object... args) throws Exception {
        Constructor<?> constructor = owner.getDeclaredConstructor(signature); constructor.setAccessible(true);
        try { return constructor.newInstance(args); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Object target, String name, Class<?>[] signature, Object... args) throws Exception {
        Class<?> owner = target instanceof Class<?> clazz ? clazz : target.getClass();
        Method method = owner.getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(target instanceof Class<?> ? null : target, args); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static final class Queue implements Executor {
        final ArrayDeque<Runnable> work = new ArrayDeque<>();
        boolean reject;
        @Override public void execute(Runnable item) {
            if (reject) throw new RejectedExecutionException("fixture executor rejected");
            work.addLast(item);
        }
        void next() { check(!work.isEmpty(), "missing queued request"); work.removeFirst().run(); }
        void drain() { while (!work.isEmpty()) next(); }
    }
    private static final class Radio {
        boolean present = true, on, consent = true, accepted = true, switchImmediately = true, interrupt;
        int reads, confirms, switches, pauses, throwOnRead = -1;
        long time;
        Runnable afterRead, atConfirm, atSwitch, atPause;
        final List<Boolean> requested = new ArrayList<>();
        Object proxy() throws Exception {
            Class<?> boundary = type("BluetoothRequests$Backend");
            return Proxy.newProxyInstance(boundary.getClassLoader(), new Class<?>[]{boundary}, (self, method, args) -> {
                return switch (method.getName()) {
                    case "adapter" -> {
                        reads++;
                        if (reads == throwOnRead) throw new IllegalStateException("fixture adapter observation failed");
                        if (afterRead != null) afterRead.run();
                        yield construct(WindowsBluetooth.Adapter.class,
                                new Class<?>[]{boolean.class, boolean.class, long.class, String.class,
                                        int.class, int.class, String.class, int.class},
                                present, on, 1L, "Explicit fixture radio", 3, 31, "fixture", 0);
                    }
                    case "confirm" -> { confirms++; if (atConfirm != null) atConfirm.run(); yield consent; }
                    case "setEnabled" -> {
                        switches++; requested.add((Boolean) args[0]);
                        if (atSwitch != null) atSwitch.run();
                        if (accepted && switchImmediately) on = (Boolean) args[0];
                        yield accepted;
                    }
                    case "nanoTime" -> time;
                    case "pause" -> {
                        pauses++;
                        if (interrupt) throw new InterruptedException("fixture worker interruption");
                        time += 1_000_000_000L;
                        if (atPause != null) atPause.run();
                        yield null;
                    }
                    case "toString" -> "Explicit Bluetooth request test backend";
                    case "hashCode" -> System.identityHashCode(self);
                    case "equals" -> self == args[0];
                    default -> throw new AssertionError("Unexpected backend operation " + method);
                };
            });
        }
    }
    private static ApkPackage code(String name, int uid, int target, String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/bluetooth/" + name + ".apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        ParsedActivityImpl activity = new ParsedActivityImpl();
        activity.setPackageName(name); activity.setName(name + ".Activity"); activity.setEnabled(true);
        parsed.addActivity(activity);
        for (String permission : permissions) parsed.addUsesPermission(
                new ParsedUsesPermissionImpl(permission, 0, java.util.Collections.emptySet()));
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed); set(apk, "mUid", uid);
        set(apk, "apkPath", "/fixture/bluetooth/" + name + ".apk");
        set(apk, "mDataDir", "/fixture/bluetooth/" + name);
        set(apk, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{1})});
        return apk;
    }
    private static final class Harness {
        final Map<String, ApkPackage> packages = new LinkedHashMap<>(), removing = new LinkedHashMap<>();
        final Queue queue = new Queue();
        final Radio radio = new Radio();
        final PackageManagerService pm;
        final Object requests;
        final Properties decisions;
        int nextUid = 12400;
        @SuppressWarnings("unchecked") Harness() throws Exception {
            pm = (PackageManagerService) UNSAFE.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", packages); set(pm, "mRemoving", removing);
            set(pm, "mTransientEnabled", true);
            set(pm, "mPackageRemoval", new ArrayList<>()); set(pm, "mPackageRemovalChecks", new ArrayList<>());
            set(pm, "mPackageChanges", new java.util.concurrent.CopyOnWriteArrayList<>());
            Object permissions = construct(type("PermissionPolicy"), new Class<?>[]{PackageManagerService.class}, pm);
            set(pm, "mPermissions", permissions);
            decisions = (Properties) get(permissions, "mDecisions"); decisions.clear();
            // Isolated metadata input: PermissionPolicy is unchanged, but this test
            // does not test parsing framework-res.apk or persistent permission IO.
            Object catalog = get(permissions, "mCatalog");
            Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>) get(catalog, "mFramework");
            for (String name : List.of(LEGACY, CONNECT, ADVERTISE)) {
                PermissionInfo info = new PermissionInfo(); info.name = name; info.packageName = "android";
                info.protectionLevel = name.equals(LEGACY) ? PermissionInfo.PROTECTION_NORMAL : PermissionInfo.PROTECTION_DANGEROUS;
                definitions.put(name, info);
            }
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            requests = construct(type("BluetoothRequests"),
                    new Class<?>[]{PackageManagerService.class, Executor.class, type("BluetoothRequests$Backend")},
                    pm, queue, radio.proxy());
        }
        ApkPackage install(String name, int target, String... requested) throws Exception {
            ApkPackage apk = code(name, nextUid++, target, requested); packages.put(name, apk); return apk;
        }
        void grant(ApkPackage apk, String permission, boolean granted) {
            decisions.setProperty("uid:" + apk.uid() + ":" + permission, granted ? "granted" : "denied");
        }
        ApkPackage modern(String name) throws Exception {
            ApkPackage apk = install(name, 36, CONNECT, ADVERTISE);
            grant(apk, CONNECT, true); grant(apk, ADVERTISE, true); return apk;
        }
        void request(String action, String name, ApkPackage apk, BooleanSupplier lifetime, IntConsumer result) throws Exception {
            invoke(requests, "request", new Class<?>[]{String.class, String.class, ApkPackage.class,
                    BooleanSupplier.class, IntConsumer.class}, action, name, apk, lifetime, result);
        }
        void request(String action, ApkPackage apk, List<Integer> results) throws Exception {
            request(action, apk.packageName, apk, () -> true, results::add);
        }
        int complete(String action, ApkPackage apk) throws Exception {
            List<Integer> results = new ArrayList<>(); request(action, apk, results);
            check(results.isEmpty(), "accepted request completed on calling thread");
            queue.next(); check(results.size() == 1, "request must complete exactly once: " + results);
            return results.get(0);
        }
    }
    private static void stateMachine() throws Exception {
        for (String action : List.of(ENABLE, DISABLE)) {
            boolean enable = action.equals(ENABLE);
            Harness already = new Harness(); ApkPackage app = already.modern("fixture.bt.already");
            already.radio.on = enable;
            check(already.complete(action, app) == Activity.RESULT_OK, "already desired state was rejected");
            check(already.radio.confirms == 0 && already.radio.switches == 0, "already state changed hardware or prompted");
            Harness absent = new Harness(); app = absent.modern("fixture.bt.absent"); absent.radio.present = false;
            check(absent.complete(action, app) == Activity.RESULT_CANCELED, "absent adapter reported success");
            check(absent.radio.confirms == 0 && absent.radio.switches == 0, "absent adapter prompted/switched");
            Harness approved = new Harness(); app = approved.modern("fixture.bt.approved"); approved.radio.on = !enable;
            check(approved.complete(action, app) == Activity.RESULT_OK, "observed approved transition failed");
            check(approved.radio.confirms == 1 && approved.radio.switches == 1
                    && approved.radio.requested.equals(List.of(enable)) && approved.radio.reads == 2,
                    "approved transition did not confirm, toggle and observe exactly once");
            Harness denied = new Harness(); app = denied.modern("fixture.bt.denied"); denied.radio.on = !enable;
            denied.radio.consent = false; denied.radio.atConfirm = () -> denied.radio.on = enable;
            check(denied.complete(action, app) == Activity.RESULT_CANCELED, "denial became success after external state change");
            check(denied.radio.switches == 0, "denied request toggled adapter");
            Harness refused = new Harness(); app = refused.modern("fixture.bt.refused"); refused.radio.on = !enable;
            refused.radio.accepted = false; refused.radio.atSwitch = () -> refused.radio.on = enable;
            check(refused.complete(action, app) == Activity.RESULT_CANCELED, "backend rejection became success");
            Harness timeout = new Harness(); app = timeout.modern("fixture.bt.timeout"); timeout.radio.on = !enable;
            timeout.radio.switchImmediately = false;
            check(timeout.complete(action, app) == Activity.RESULT_CANCELED, "request acceptance was mistaken for completed transition");
            check(timeout.radio.time >= 10_000_000_000L && timeout.radio.pauses <= 11, "transition wait was not bounded");
            Harness delayed = new Harness(); app = delayed.modern("fixture.bt.delayed"); delayed.radio.on = !enable;
            delayed.radio.switchImmediately = false;
            delayed.radio.atPause = () -> { if (delayed.radio.pauses == 3) delayed.radio.on = enable; };
            check(delayed.complete(action, app) == Activity.RESULT_OK && delayed.radio.pauses == 3, "delayed transition was not observed");
            Harness lost = new Harness(); app = lost.modern("fixture.bt.lost"); lost.radio.on = !enable;
            lost.radio.atSwitch = () -> lost.radio.present = false;
            check(lost.complete(action, app) == Activity.RESULT_CANCELED, "lost radio reported success");
            Harness broken = new Harness(); app = broken.modern("fixture.bt.broken"); broken.radio.on = !enable;
            broken.radio.throwOnRead = 2;
            check(broken.complete(action, app) == Activity.RESULT_CANCELED, "failed observation did not complete as canceled");
            Harness interrupted = new Harness(); app = interrupted.modern("fixture.bt.interrupted"); interrupted.radio.on = !enable;
            interrupted.radio.switchImmediately = false; interrupted.radio.interrupt = true;
            check(interrupted.complete(action, app) == Activity.RESULT_CANCELED, "interrupted transition did not cancel");
            check(Thread.interrupted(), "worker did not preserve interruption");
        }
        for (boolean initiallyOn : List.of(false, true)) {
            Harness h = new Harness(); ApkPackage app = h.modern("fixture.bt.discoverable"); h.radio.on = initiallyOn;
            check(h.complete(DISCOVERABLE, app) == Activity.RESULT_CANCELED, "unsupported discoverability fabricated a duration");
            check(h.radio.reads == 0 && h.radio.confirms == 0 && h.radio.switches == 0,
                    "discoverable request substituted radio power or opened misleading consent");
        }
    }
    private static void permissionsAndLifetimes() throws Exception {
        Harness h = new Harness(); ApkPackage none = h.install("fixture.bt.no.permission", 36);
        for (String action : List.of(ENABLE, DISABLE, DISCOVERABLE))
            expect(SecurityException.class, () -> h.request(action, none, new ArrayList<>()), "modern permission bypass");
        check(h.queue.work.isEmpty() && h.radio.reads == 0, "permission rejection queued host work");
        expect(SecurityException.class, () -> h.request(ENABLE, "uninstalled", null, () -> true, result -> {}), "uninstalled caller accepted");
        List<Integer> unknown = new ArrayList<>();
        h.request("unknown", "android", null, () -> true, unknown::add); h.queue.next();
        check(unknown.equals(List.of(Activity.RESULT_CANCELED)) && h.radio.reads == 0,
                "unknown delivered selector action performed a radio operation");
        Harness onlyConnect = new Harness(); ApkPackage connect = onlyConnect.install("fixture.bt.connect", 36, CONNECT);
        onlyConnect.grant(connect, CONNECT, true);
        expect(SecurityException.class, () -> onlyConnect.request(DISCOVERABLE, connect, new ArrayList<>()), "discoverability accepted CONNECT without ADVERTISE");
        Harness onlyAdvertise = new Harness(); ApkPackage advertise = onlyAdvertise.install("fixture.bt.advertise", 36, ADVERTISE);
        onlyAdvertise.grant(advertise, ADVERTISE, true);
        expect(SecurityException.class, () -> onlyAdvertise.request(ENABLE, advertise, new ArrayList<>()), "enable accepted ADVERTISE without CONNECT");
        Harness legacy = new Harness(); ApkPackage old = legacy.install("fixture.bt.legacy", 30, LEGACY);
        check(legacy.complete(ENABLE, old) == Activity.RESULT_OK, "legacy BLUETOOTH consent path rejected");
        check(legacy.complete(DISABLE, old) == Activity.RESULT_OK, "legacy BLUETOOTH disable path rejected");
        for (String phase : List.of("queued", "consent", "wait")) {
            Harness revoke = new Harness(); ApkPackage app = revoke.modern("fixture.bt.revoked");
            List<Integer> results = new ArrayList<>();
            if (phase.equals("consent")) revoke.radio.atConfirm = () -> revoke.grant(app, CONNECT, false);
            if (phase.equals("wait")) {
                revoke.radio.switchImmediately = false; revoke.radio.atPause = () -> revoke.grant(app, CONNECT, false);
            }
            revoke.request(ENABLE, app, results);
            if (phase.equals("queued")) revoke.grant(app, CONNECT, false);
            revoke.queue.next();
            check(results.equals(List.of(Activity.RESULT_CANCELED)), "revoked grant succeeded at " + phase);
            check(revoke.radio.switches == (phase.equals("wait") ? 1 : 0), "grant ignored before switch at " + phase);
        }
        for (String retirement : List.of("lifetime", "removed", "replaced", "removing", "disabled")) {
            Harness stale = new Harness(); ApkPackage app = stale.modern("fixture.bt.retired");
            boolean[] alive = {true}; List<Integer> results = new ArrayList<>();
            stale.request(ENABLE, app.packageName, app, () -> alive[0], results::add);
            switch (retirement) {
                case "lifetime" -> alive[0] = false;
                case "removed" -> stale.packages.remove(app.packageName);
                case "replaced" -> stale.packages.put(app.packageName, code(app.packageName, app.uid(), 36, CONNECT));
                case "removing" -> stale.removing.put(app.packageName, app);
                case "disabled" -> ((PackageImpl) app.parsed).setEnabled(false);
            }
            stale.queue.next();
            check(results.equals(List.of(Activity.RESULT_CANCELED)) && stale.radio.reads == 0, "stale owner touched radio: " + retirement);
        }
        Harness stale = new Harness(); ApkPackage app = stale.modern("fixture.bt.prompt.retire");
        boolean[] alive = {true}; List<Integer> results = new ArrayList<>();
        stale.radio.atConfirm = () -> alive[0] = false;
        stale.request(ENABLE, app.packageName, app, () -> alive[0], results::add); stale.queue.next();
        check(results.equals(List.of(Activity.RESULT_CANCELED)) && stale.radio.switches == 0, "retired process approved pending dialog");
        for (int read : List.of(1, 2)) {
            Harness revokeDuringRead = new Harness(); ApkPackage owner = revokeDuringRead.modern("fixture.bt.read.revoke");
            revokeDuringRead.radio.afterRead = () -> {
                if (revokeDuringRead.radio.reads == read) revokeDuringRead.grant(owner, CONNECT, false);
            };
            check(revokeDuringRead.complete(ENABLE, owner) == Activity.RESULT_CANCELED,
                    "grant revoked inside adapter observation escaped validation");
            check(revokeDuringRead.radio.switches == (read == 1 ? 0 : 1), "revoked observation permitted an extra mutation");
        }
    }
    private static void admission() throws Exception {
        Harness h = new Harness(); ApkPackage app = h.modern("fixture.bt.duplicate");
        List<Integer> first = new ArrayList<>(), second = new ArrayList<>();
        h.request(ENABLE, app, first); h.request(DISABLE, app, second);
        check(second.equals(List.of(Activity.RESULT_CANCELED)) && first.isEmpty() && h.queue.work.size() == 1,
                "same package queued multiple dialogs or replaced its first request");
        h.queue.next(); check(first.equals(List.of(Activity.RESULT_OK)), "first admitted request lost");
        check(h.complete(DISABLE, app) == Activity.RESULT_OK, "completion did not release admission");
        Harness bounded = new Harness(); List<Integer> all = new ArrayList<>();
        for (int i = 0; i < 17; i++) bounded.request(ENABLE, bounded.modern("fixture.bt.limit" + i), all);
        check(bounded.queue.work.size() == 16 && all.equals(List.of(Activity.RESULT_CANCELED)), "pending request bound not enforced");
        bounded.radio.present = false; bounded.queue.drain(); check(all.size() == 17, "bounded requests did not all complete");
        check(bounded.complete(ENABLE, bounded.packages.get("fixture.bt.limit0")) == Activity.RESULT_CANCELED, "drained queue retained admission");
        Harness rejected = new Harness(); app = rejected.modern("fixture.bt.rejected"); rejected.queue.reject = true;
        List<Integer> canceled = new ArrayList<>(); rejected.request(ENABLE, app, canceled);
        check(canceled.equals(List.of(Activity.RESULT_CANCELED)), "executor rejection lost completion");
        rejected.queue.reject = false;
        check(rejected.complete(ENABLE, app) == Activity.RESULT_OK, "executor rejection leaked admission");
        Object direct = construct(type("BluetoothRequests"),
                new Class<?>[]{PackageManagerService.class, Executor.class, type("BluetoothRequests$Backend")},
                rejected.pm, (Executor) Runnable::run, rejected.radio.proxy());
        int[] completions = {0}; final ApkPackage directOwner = app;
        expect(IllegalStateException.class, () -> invoke(direct, "request",
                new Class<?>[]{String.class, String.class, ApkPackage.class, BooleanSupplier.class, IntConsumer.class},
                ENABLE, directOwner.packageName, directOwner, (BooleanSupplier) () -> true,
                (IntConsumer) result -> { completions[0]++; throw new IllegalStateException("fixture callback"); }),
                "direct executor swallowed completion exception");
        check(completions[0] == 1, "completion exception was mistaken for executor rejection and delivered twice");
    }
    private static boolean handles(Intent intent, String resolvedType) throws Exception {
        return (boolean) invoke(type("BluetoothRequests"), "handles", new Class<?>[]{Intent.class, String.class}, intent, resolvedType);
    }
    private static void matching() throws Exception {
        for (String action : List.of(ENABLE, DISABLE, DISCOVERABLE)) {
            for (String pkg : new String[]{null, "com.android.settings", "com.android.bluetooth"})
                check(handles(new Intent(action).setPackage(pkg), null), "valid request not handled");
            check(handles(new Intent(action).addCategory(Intent.CATEGORY_DEFAULT), null), "DEFAULT category rejected");
        }
        Intent selector = new Intent("fixture.outer"); selector.setSelector(new Intent(ENABLE));
        check(handles(selector, null), "valid selector request rejected");
        Intent[] rejected = {null, new Intent(), new Intent("fixture.other"),
                new Intent(ENABLE).setPackage("other.application"),
                new Intent(ENABLE).setComponent(new ComponentName("other.application", "Missing")),
                new Intent(ENABLE).setData(Uri.parse("content://unrelated/item")), new Intent(ENABLE).setType("text/plain"),
                new Intent(ENABLE).addCategory(Intent.CATEGORY_BROWSABLE),
                new Intent(ENABLE).addCategory(Intent.CATEGORY_DEFAULT).addCategory(Intent.CATEGORY_BROWSABLE)};
        for (Intent intent : rejected) check(!handles(intent, null), "malformed/foreign request intercepted: " + intent);
        check(!handles(new Intent(ENABLE), "text/plain"), "resolved MIME type ignored");
        Intent foreign = new Intent(ENABLE); foreign.setSelector(new Intent(ENABLE).setPackage("foreign.app"));
        check(!handles(foreign, null), "foreign selector intercepted");
        selector.setComponent(new ComponentName("other.application", "Missing"));
        check(!handles(selector, null), "outer explicit component ignored");
    }
    private static void routing() throws Exception {
        Harness h = new Harness();
        Object atm = construct(ActivityTaskManagerService.class, new Class<?>[]{PackageManagerService.class}, h.pm);
        set(atm, "mBluetoothRequests", h.requests);
        Method start = null;
        for (Method method : atm.getClass().getDeclaredMethods())
            if (method.getName().equals("start") && method.getParameterCount() == 8) start = method;
        check(start != null, "eight-argument activity starter unavailable"); start.setAccessible(true);
        for (String action : List.of(ENABLE, DISABLE, DISCOVERABLE)) {
            Intent intent = new Intent(action).setPackage("com.android.settings");
            if (action.equals(DISCOVERABLE)) intent.putExtra("android.bluetooth.adapter.extra.DISCOVERABLE_DURATION", "invalid-type");
            Object status = start.invoke(atm, null, intent, null, "android", null, null, -1, null);
            check(Integer.valueOf(ActivityManager.START_SUCCESS).equals(status), "actual activity route rejected " + action);
            check(h.queue.work.size() == 1, "actual route did not enqueue helper");
            check(((java.util.Set<?>) get(atm, "mBluetoothActivities")).size() == 1,
                    "native request did not reserve its idle lifetime");
            check((long) invoke(atm, "idleSince", new Class<?>[]{}) == 0,
                    "runtime went idle while native request was pending");
            h.queue.next();
            check(((java.util.Set<?>) get(atm, "mBluetoothActivities")).isEmpty(),
                    "native request completion leaked idle reservation");
        }
        h.radio.on = true;
        Intent opposite = new Intent(DISABLE); opposite.setSelector(new Intent(ENABLE));
        check(Integer.valueOf(ActivityManager.START_SUCCESS).equals(
                start.invoke(atm, null, opposite, null, "android", null, null, -1, null)), "selector route rejected");
        h.queue.next();
        check(!h.radio.on && h.radio.requested.get(h.radio.requested.size() - 1).equals(false),
                "selector action replaced delivered outer action");
        int beforeSelector = h.radio.reads + h.radio.confirms + h.radio.switches;
        Intent unknown = new Intent("fixture.unknown.outer"); unknown.setSelector(new Intent(ENABLE));
        check(Integer.valueOf(ActivityManager.START_SUCCESS).equals(
                start.invoke(atm, null, unknown, null, "android", null, null, -1, null)), "unknown outer selector route rejected");
        h.queue.next();
        check(beforeSelector == h.radio.reads + h.radio.confirms + h.radio.switches,
                "unknown outer action executed Bluetooth selector instead");
        Field opener = field(ActivityTaskManagerService.class, "sOpenExternalUrl");
        Object saved = opener.get(null); int[] urls = {0};
        try {
            opener.set(null, (java.util.function.Predicate<String>) url -> { urls[0]++; return true; });
            Intent view = new Intent(Intent.ACTION_VIEW, Uri.parse("https://fixture.invalid/"));
            view.setSelector(new Intent(ENABLE));
            check(Integer.valueOf(ActivityManager.START_SUCCESS).equals(
                    start.invoke(atm, null, view, null, "android", null, null, -1, null)), "VIEW selector route rejected");
            h.queue.next();
            check(urls[0] == 0 && beforeSelector == h.radio.reads + h.radio.confirms + h.radio.switches,
                    "VIEW outer action bypassed Bluetooth selector routing");
        } finally { opener.set(null, saved); }
        int operations = h.radio.reads + h.radio.confirms + h.radio.switches;
        for (Intent intent : List.of(new Intent(ENABLE).setPackage("other.application"),
                new Intent(ENABLE).setComponent(new ComponentName("other.application", "Missing")),
                new Intent(ENABLE).setData(Uri.parse("content://foreign/item")), new Intent(ENABLE).setType("text/plain"),
                new Intent(ENABLE).addCategory(Intent.CATEGORY_BROWSABLE))) {
            Object status = start.invoke(atm, null, intent, null, "android", null, null, -1, null);
            check(!Integer.valueOf(ActivityManager.START_SUCCESS).equals(status), "actual route swallowed " + intent);
            check(h.queue.work.isEmpty(), "foreign/malformed actual request queued work");
        }
        check(operations == h.radio.reads + h.radio.confirms + h.radio.switches, "unhandled intent reached backend");
    }
    /** Result/lifetime integration, using real ActivityRecord and pending ResultInfo objects. */
    private static final class ResultRoute {
        final Harness h = new Harness();
        final ApkPackage owner = h.modern("fixture.bt.results");
        final Object atm = construct(ActivityTaskManagerService.class, new Class<?>[]{PackageManagerService.class}, h.pm);
        final Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
        final Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
        int nextPid = 97501, nextTask = 1;
        ResultRoute() throws Exception {
            set(atm, "mBluetoothRequests", h.requests);
            // The real result publisher needs its URI-grant boundary. Empty result
            // data carries no URIs; unrelated service constructors are unnecessary.
            Object providers = UNSAFE.allocateInstance(type("ContentProviders"));
            Object am = UNSAFE.allocateInstance(ActivityManagerService.class);
            set(am, "mProviders", providers); set(am, "mAtm", atm); set(am, "mPm", h.pm);
            set(atm, "mAm", am);
        }
        @SuppressWarnings({"unchecked", "rawtypes"}) Object record(Object parent, String who, int code) throws Exception {
            Object process = UNSAFE.allocateInstance(processType);
            set(process, "pid", nextPid++); set(process, "uid", owner.uid());
            set(process, "packageName", owner.packageName); set(process, "processName", owner.packageName);
            set(process, "packageCode", owner); set(process, "ready", true);
            set(process, "activities", new ArrayList<>());
            ((List<Object>) get(atm, "mProcesses")).add(process);
            ActivityInfo info = new ActivityInfo(); info.packageName = owner.packageName;
            info.name = owner.packageName + ".Activity"; info.applicationInfo = owner.applicationInfo(0);
            info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            int task = nextTask++;
            Object record = construct(recordType, new Class<?>[]{ActivityTaskManagerService.class,
                    ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                    recordType, String.class, int.class}, atm, info, new Intent(), task,
                    (int) get(process, "pid"), owner.packageName, parent, who, code);
            set(record, "app", process);
            set(record, "state", Enum.valueOf((Class) type("ActivityTaskManagerService$State"), "STOPPED"));
            ((List<Object>) get(atm, "mActivities")).add(record);
            ((List<Object>) get(process, "activities")).add(record);
            ((List<Integer>) get(atm, "mTaskOrder")).add(task);
            // Keep the receiver stopped so the actual pending-result queue can be
            // inspected without claiming a rendered window or Android client loop.
            ((java.util.Set<Integer>) get(atm, "mHiddenTasks")).add(task);
            return record;
        }
        int request(Object source, Intent intent, int code) throws Exception {
            return (int) invoke(atm, "requestBluetoothState", new Class<?>[]{Intent.class, String.class,
                    processType, boolean.class, BooleanSupplier.class, BooleanSupplier.class,
                    IBinder.class, String.class, int.class}, intent, owner.packageName, get(source, "app"),
                    false, null, null, get(source, "token"), "request-who", code);
        }
        @SuppressWarnings("unchecked") List<ResultInfo> results(Object record) throws Exception {
            return (List<ResultInfo>) get(record, "pendingResults");
        }
        void released(Object source) throws Exception {
            check((int) get(source, "nativeUiCount") == 0, "native UI cover count leaked");
            check(((java.util.Set<?>) get(atm, "mBluetoothActivities")).isEmpty(), "native UI idle reservation leaked");
        }
    }
    private static void resultRoutes() throws Exception {
        ResultRoute normal = new ResultRoute(); Object source = normal.record(null, null, -1);
        check(normal.request(source, new Intent(ENABLE), 42) == ActivityManager.START_SUCCESS, "result launch rejected");
        check(normal.results(source).isEmpty() && (int) get(source, "nativeUiCount") == 1,
                "result published before queued work or activity was not covered");
        normal.h.queue.next(); normal.released(source);
        List<ResultInfo> results = normal.results(source);
        check(results.size() == 1 && results.get(0).mResultCode == Activity.RESULT_OK
                && results.get(0).mRequestCode == 42 && "request-who".equals(results.get(0).mResultWho),
                "normal result lost status or routing fields");
        check(results.get(0).mCallerToken != null, "server result has no caller attribution token");
        Object snapshot = invoke(get(source, "callerState"), "get", new Class<?>[]{IBinder.class}, results.get(0).mCallerToken);
        check("android".equals(get(snapshot, "packageName")), "worker result attributed to application instead of server");

        ResultRoute replaced = new ResultRoute(); source = replaced.record(null, null, -1);
        replaced.request(source, new Intent(ENABLE), 7);
        Object other = replaced.record(null, null, -1); set(source, "app", get(other, "app"));
        replaced.h.queue.next(); replaced.released(source);
        check(replaced.results(source).isEmpty() && replaced.h.radio.reads == 0,
                "old activity request was redirected to replacement process");

        ResultRoute detached = new ResultRoute(); source = detached.record(null, null, -1);
        detached.request(source, new Intent(ENABLE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), 8);
        check(detached.results(source).size() == 1
                && detached.results(source).get(0).mResultCode == Activity.RESULT_CANCELED,
                "NEW_TASK did not detach result immediately");
        set(source, "finishing", true); set(get(source, "app"), "dead", true);
        detached.h.queue.next(); detached.released(source);
        check(detached.h.radio.switches == 1 && detached.h.radio.on && detached.results(source).size() == 1,
                "finishing detached source canceled accepted work or received a second result");

        ResultRoute forward = new ResultRoute(); Object recipient = forward.record(null, null, -1);
        source = forward.record(recipient, "forwarded-who", 73);
        Intent forwarded = new Intent(ENABLE).addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT);
        check(forward.request(source, forwarded, 4) == ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT,
                "forward+new result did not report conflict");
        check(get(source, "resultTo") == recipient && forward.h.queue.work.isEmpty(),
                "conflicting start consumed saved result or queued work");
        check(forward.request(source, forwarded, -1) == ActivityManager.START_SUCCESS, "forwarded result request rejected");
        check(get(source, "resultTo") == null, "forwarded route was not consumed exactly once");
        set(source, "finishing", true); set(get(source, "app"), "dead", true); set(source, "app", null);
        forward.h.queue.next(); forward.released(source);
        results = forward.results(recipient);
        check(forward.h.radio.switches == 1 && results.size() == 1 && results.get(0).mResultCode == Activity.RESULT_OK
                && results.get(0).mRequestCode == 73 && "forwarded-who".equals(results.get(0).mResultWho),
                "trampoline death lost saved recipient's forwarded result");
    }

    /** The real starter must retain the admitted APK's identity in standalone mode. */
    private static final class StandaloneRoute {
        final Harness h = new Harness();
        final ApkPackage owner = h.modern("fixture.bt.standalone");
        final Object atm = construct(ActivityTaskManagerService.class,
                new Class<?>[]{PackageManagerService.class}, h.pm);
        final Object process;
        final IApplicationThread thread = appThread();
        final Method starter;

        static IApplicationThread appThread() {
            return new IApplicationThread.Default() {
                private final IBinder token = new Binder();
                @Override public IBinder asBinder() { return token; }
            };
        }

        @SuppressWarnings({"unchecked", "rawtypes"}) StandaloneRoute() throws Exception {
            set(atm, "mBluetoothRequests", h.requests);
            Object am = UNSAFE.allocateInstance(ActivityManagerService.class);
            set(am, "mAtm", atm); set(am, "mPm", h.pm);
            set(am, "mCallers", new CallerIdentity(h.pm, (ActivityTaskManagerService) atm));
            set(atm, "mAm", am);
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            process = UNSAFE.allocateInstance(processType);
            set(process, "pid", android.os.Process.myPid()); set(process, "uid", owner.uid());
            set(process, "packageName", owner.packageName); set(process, "processName", owner.packageName);
            set(process, "packageCode", owner); set(process, "ready", true); set(process, "thread", thread);
            set(process, "activities", new ArrayList<>());
            set(process, "inProcessActivityState",
                    Enum.valueOf((Class) type("ActivityTaskManagerService$State"), "STOPPED"));
            ((List<Object>) get(atm, "mProcesses")).add(process);
            Method selected = null;
            for (Method method : atm.getClass().getDeclaredMethods())
                if (method.getName().equals("start") && method.getParameterCount() == 8) selected = method;
            if (selected == null) throw new AssertionError("Activity starter unavailable");
            starter = selected; starter.setAccessible(true);
        }

        int launch(String action, String claimed, IApplicationThread caller) throws Exception {
            long saved = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) owner.uid() << 32)
                        | (android.os.Process.myPid() & 0xffffffffL));
                check(Binder.getCallingPid() == android.os.Process.myPid()
                        && Binder.getCallingUid() == owner.uid(), "standalone fixture identity unavailable");
                try {
                    return (int) starter.invoke(atm, caller, new Intent(action), null,
                            claimed, null, null, -1, null);
                } catch (InvocationTargetException failure) { return (int) rethrow(failure.getCause()); }
            } finally { Binder.restoreCallingIdentity(saved); }
        }

        void untouched() {
            check(h.queue.work.isEmpty() && h.radio.reads == 0 && h.radio.confirms == 0
                    && h.radio.switches == 0, "declined standalone launch reached host work");
        }
    }

    @SuppressWarnings("unchecked") private static void standaloneIdentity() throws Exception {
        StandaloneRoute denied = new StandaloneRoute();
        denied.h.grant(denied.owner, CONNECT, false); denied.h.grant(denied.owner, ADVERTISE, false);
        for (String action : List.of(ENABLE, DISABLE, DISCOVERABLE))
            expect(SecurityException.class, () -> denied.launch(action, denied.owner.packageName, denied.thread),
                    "standalone caller did not retain its permission requirements");
        denied.untouched();

        StandaloneRoute allowed = new StandaloneRoute();
        check(allowed.launch(ENABLE, allowed.owner.packageName, allowed.thread) == ActivityManager.START_SUCCESS,
                "granted standalone caller was rejected");
        allowed.h.queue.next();
        check(allowed.h.radio.on && allowed.h.radio.switches == 1, "granted standalone operation was lost");

        StandaloneRoute identity = new StandaloneRoute();
        expect(SecurityException.class, () -> identity.launch(ENABLE, "android", identity.thread),
                "standalone package claim replaced its admitted owner");
        expect(SecurityException.class, () -> identity.launch(ENABLE, identity.owner.packageName,
                StandaloneRoute.appThread()), "standalone caller thread did not match its admission");
        expect(SecurityException.class, () -> identity.launch(ENABLE, identity.owner.packageName, null),
                "standalone launch without an admitted thread was accepted");
        identity.untouched();

        for (String state : List.of("absent", "exiting", "dead", "unready", "replaced", "removing", "disabled")) {
            StandaloneRoute retired = new StandaloneRoute();
            switch (state) {
                case "absent" -> ((List<?>) get(retired.atm, "mProcesses")).clear();
                case "exiting" -> set(retired.process, "exiting", true);
                case "dead" -> set(retired.process, "dead", true);
                case "unready" -> set(retired.process, "ready", false);
                case "replaced" -> retired.h.packages.put(retired.owner.packageName,
                        code(retired.owner.packageName, retired.owner.uid(), 36, CONNECT));
                case "removing" -> retired.h.removing.put(retired.owner.packageName, retired.owner);
                case "disabled" -> ((PackageImpl) retired.owner.parsed).setEnabled(false);
            }
            expect(SecurityException.class, () -> retired.launch(ENABLE, retired.owner.packageName, retired.thread),
                    "standalone admission was no longer current: " + state);
            retired.untouched();
        }

        StandaloneRoute published = new StandaloneRoute();
        published.h.grant(published.owner, CONNECT, false);
        ThreadLocal<String> creator = (ThreadLocal<String>) get(published.atm, "mVerifiedCreator");
        try {
            creator.set(published.owner.packageName);
            expect(SecurityException.class, () -> published.launch(ENABLE, "android", null),
                    "verified application creator lost its permission requirements");
            published.untouched();
            creator.set("android");
            check(published.launch(ENABLE, "android", null) == ActivityManager.START_SUCCESS,
                    "verified system creator was changed to the local application's identity");
            published.h.queue.next();
            check(published.h.radio.on, "verified system request did not complete");
        } finally { creator.remove(); }
    }

    public static int run() throws Exception {
        checks = 0; stateMachine(); permissionsAndLifetimes(); admission(); matching(); routing();
        resultRoutes(); standaloneIdentity(); return checks;
    }
}
