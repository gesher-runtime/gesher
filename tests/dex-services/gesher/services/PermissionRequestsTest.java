/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiPredicate;
import java.util.function.Consumer;
import java.util.function.ToIntBiFunction;

/** Server-local consent injection; no Windows security prompt is automated. */
public final class PermissionRequestsTest {
    private static final String LOCATION = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String BACKGROUND = "android.permission.ACCESS_BACKGROUND_LOCATION";
    // PermissionRequests' dialog protocol values.
    private static final int UNAVAILABLE = -1, DISMISSED = 0, DENIED = 1, ALLOWED = 2,
            FOREGROUND_ONLY = 3, ALWAYS = 4;
    private static final String CAMERA = "android.permission.CAMERA";
    private static final String INTERNET = "android.permission.INTERNET";
    private static final String PRIVILEGED = "android.permission.MANAGE_USB";
    private static final String UNKNOWN = "com.gesher.permission.NOT_DECLARED";

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        Method method = type.getDeclaredMethod(name, types);
        method.setAccessible(true);
        return method;
    }

    private static Object controller(Object pm, Class<?> pmClass, Executor executor,
            BiPredicate<String, String> decision) throws Exception {
        return controller(pm, pmClass, executor, decision, (owner, names) -> DENIED);
    }

    private interface ReviewHandler {
        int review(String owner, String[] groups, boolean[] enabled);
    }

    private static Object controller(Object pm, Class<?> pmClass, Executor executor,
            BiPredicate<String, String> decision, ToIntBiFunction<String, String> background) throws Exception {
        return controller(pm, pmClass, executor, decision, background, (owner, groups, enabled) -> UNAVAILABLE);
    }

    private static Object controller(Object pm, Class<?> pmClass, Executor executor,
            BiPredicate<String, String> decision, ToIntBiFunction<String, String> background,
            ReviewHandler review) throws Exception {
        Class<?> requests = Class.forName("com.gesher.server.PermissionRequests");
        Class<?> choices = Class.forName("com.gesher.server.PermissionRequests$Decision");
        Object choice = Proxy.newProxyInstance(choices.getClassLoader(), new Class<?>[]{choices},
                (proxy, called, args) -> switch (called.getName()) {
                    case "decideBackground" -> background.applyAsInt((String) args[0], (String) args[1]);
                    case "decideReview" -> review.review((String) args[0], (String[]) args[1], (boolean[]) args[2]);
                    default -> decision.test((String) args[0], (String) args[1]) ? ALLOWED : DENIED;
                });
        Constructor<?> constructor = requests.getDeclaredConstructor(pmClass, Executor.class, choices);
        constructor.setAccessible(true);
        return constructor.newInstance(pm, executor, choice);
    }

    private static void request(Object controller, Object apk, Class<?> apkClass,
            String[] names, AtomicReference<Intent> result) throws Exception {
        Consumer<Intent> completed = result::set;
        method(controller.getClass(), "request", apkClass, String[].class, Consumer.class)
                .invoke(controller, apk, names, completed);
    }

    private static int grant(Object policy, String pkg, String permission) throws Exception {
        return (Integer) policy.getClass().getMethod("checkPermission", String.class, String.class)
                .invoke(policy, pkg, permission);
    }

    private static void revoke(Object policy, String pkg) throws Exception {
        revoke(policy, pkg, LOCATION);
    }

    private static void revoke(Object policy, String pkg, String permission) throws Exception {
        policy.getClass().getMethod("revokeRuntimePermission", String.class, String.class,
                String.class, int.class, String.class)
                .invoke(policy, pkg, permission, "default:0", 0, "regression test");
    }

    /**
     * The unmodified GmsCore manifest (target 29) requests foreground and background
     * location together: the foreground Allow never grants background access, a
     * separate background choice follows, and its own App info offers that choice
     * again before returning, instead of opening the Control Center.
     */
    private static void background(Object pm, Class<?> pmClass, Class<?> apkClass, Object apk,
            String pkg, Object policy) throws Exception {
        Object parsed = apkClass.getField("parsed").get(apk);
        @SuppressWarnings("unchecked") java.util.Set<String> requested = (java.util.Set<String>)
                parsed.getClass().getMethod("getRequestedPermissions").invoke(parsed);
        if (!requested.contains(BACKGROUND) || !requested.contains(FINE)) {
            System.out.println("PermissionRequestsTest: SKIP background consent (APK does not request background location)");
            return;
        }
        for (String permission : new String[]{LOCATION, FINE, BACKGROUND}) revoke(policy, pkg, permission);
        policy.getClass().getMethod("updatePermissionFlags", String.class, String.class, int.class, int.class,
                boolean.class, String.class, int.class).invoke(policy, pkg, BACKGROUND,
                PackageManager.MASK_PERMISSION_FLAGS_ALL, PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT,
                true, "default:0", 0);
        List<String> foreground = new ArrayList<>(), background = new ArrayList<>();
        Object whileUsing = controller(pm, pmClass, Runnable::run, (owner, names) -> { foreground.add(names); return true; },
                (owner, names) -> { background.add(names); return FOREGROUND_ONLY; });
        AtomicReference<Intent> reply = new AtomicReference<>();
        request(whileUsing, apk, apkClass, new String[]{LOCATION, FINE, BACKGROUND}, reply);
        int[] grants = reply.get().getIntArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS);
        require(foreground.size() == 1 && !foreground.get(0).contains(BACKGROUND) && background.size() == 1
                && background.get(0).contains(BACKGROUND), "foreground and background were not asked separately");
        require(grants[0] == PackageManager.PERMISSION_GRANTED && grants[1] == PackageManager.PERMISSION_GRANTED
                && grants[2] == PackageManager.PERMISSION_DENIED, "foreground Allow granted background location");
        // A second "Allow only while using the app" in the request dialog fixes the refusal;
        // App info (the Settings page) must still offer the choice, or microG loops.
        request(whileUsing, apk, apkClass, new String[]{LOCATION, FINE, BACKGROUND}, reply);
        int fixed = (Integer) policy.getClass().getMethod("getPermissionFlags", String.class, String.class,
                String.class, int.class).invoke(policy, pkg, BACKGROUND, "default:0", 0);
        require(background.size() == 2 && (fixed & PackageManager.FLAG_PERMISSION_USER_FIXED) != 0,
                "a repeated background refusal in the request dialog was not fixed");
        List<Runnable> queue = new ArrayList<>();
        Object settings = controller(pm, pmClass, queue::add, (owner, names) -> false,
                (owner, names) -> { background.add(names); return ALWAYS; });
        AtomicInteger returned = new AtomicInteger();
        Method open = method(settings.getClass(), "backgroundSettings", apkClass, Runnable.class);
        require((Boolean) open.invoke(settings, apk, (Runnable) returned::incrementAndGet)
                && returned.get() == 0 && queue.size() == 1, "App info did not wait for the background choice");
        queue.remove(0).run();
        require(returned.get() == 1 && background.size() == 3
                && grant(policy, pkg, BACKGROUND) == PackageManager.PERMISSION_GRANTED,
                "App info did not grant background location after the choice");
        require(!(Boolean) open.invoke(settings, apk, (Runnable) returned::incrementAndGet) && queue.isEmpty(),
                "App info offered a background choice that was already made");
        for (String permission : new String[]{LOCATION, FINE, BACKGROUND}) revoke(policy, pkg, permission);
        System.out.println("PermissionRequestsTest: separate background consent and App info choice passed");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> identityClass = Class.forName("com.gesher.server.CallerIdentity");
        Object apk = apkClass.getMethod("parse", String.class).invoke(null, args[0]);
        String pkg = (String) apkClass.getField("packageName").get(apk);
        Object pm = pmClass.getConstructor(java.util.Collection.class)
                .newInstance(Collections.singletonList(apk));
        Object policy = pmClass.getMethod("permissions").invoke(pm);
        Object identity = identityClass.getConstructor(pmClass, atmClass).newInstance(pm, null);
        method(policy.getClass(), "attach", identityClass).invoke(policy, identity);
        require((Boolean) policy.getClass().getMethod("canRequest", String.class, String.class)
                .invoke(policy, pkg, LOCATION), "fixture does not request a dangerous location permission");
        require(!(Boolean) policy.getClass().getMethod("canRequest", String.class, String.class)
                .invoke(policy, pkg, PRIVILEGED), "privileged permission became consent-requestable");
        revoke(policy, pkg);
        String[] names = {LOCATION, INTERNET, PRIVILEGED, UNKNOWN, LOCATION};
        AtomicReference<Intent> reply = new AtomicReference<>();
        AtomicInteger prompts = new AtomicInteger();
        Object denied = controller(pm, pmClass, Runnable::run, (name, description) -> {
            require(pkg.equals(name), "prompt has the wrong owner");
            require(description.contains(LOCATION) && !description.contains(PRIVILEGED)
                    && !description.contains(UNKNOWN), "prompt included non-requestable permissions");
            prompts.incrementAndGet();
            return false;
        });
        request(denied, apk, apkClass, names, reply);
        int[] deniedResults = reply.get().getIntArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS);
        require(prompts.get() == 1 && deniedResults.length == names.length,
                "denied request lost its result mapping");
        require(deniedResults[0] == PackageManager.PERMISSION_DENIED
                        && deniedResults[1] == PackageManager.PERMISSION_GRANTED
                        && deniedResults[2] == PackageManager.PERMISSION_DENIED
                        && deniedResults[3] == PackageManager.PERMISSION_DENIED,
                "denial granted a dangerous, privileged or undeclared permission");
        require(grant(policy, pkg, LOCATION) == PackageManager.PERMISSION_DENIED, "denial persisted a grant");

        List<Runnable> queue = new ArrayList<>();
        Object allowed = controller(pm, pmClass, queue::add, (name, description) -> true);
        reply.set(null);
        String[] captured = names.clone();
        request(allowed, apk, apkClass, captured, reply);
        captured[0] = UNKNOWN;
        require(reply.get() == null && queue.size() == 1, "consent blocked or completed on the caller thread");
        queue.remove(0).run();
        Intent result = reply.get();
        int[] grants = result.getIntArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS);
        require(LOCATION.equals(result.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES)[0]),
                "caller mutated the asynchronous permission snapshot");
        require(grants[0] == PackageManager.PERMISSION_GRANTED && grants[4] == PackageManager.PERMISSION_GRANTED
                        && grants[2] == PackageManager.PERMISSION_DENIED && grants[3] == PackageManager.PERMISSION_DENIED,
                "consent exceeded the declared dangerous permission boundary");
        revoke(policy, pkg);

        Object replacement = apkClass.getMethod("parse", String.class).invoke(null, args[0]);
        Object stale = controller(pm, pmClass, Runnable::run, (name, description) -> {
            try { method(pmClass, "add", apkClass).invoke(pm, replacement); }
            catch (Exception e) { throw new AssertionError(e); }
            return true;
        });
        reply.set(null);
        request(stale, apk, apkClass, new String[]{LOCATION}, reply);
        require(reply.get().getIntArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS)[0]
                        == PackageManager.PERMISSION_DENIED
                        && grant(policy, pkg, LOCATION) == PackageManager.PERMISSION_DENIED,
                "replacement inherited an in-flight consent grant");
        background(pm, pmClass, apkClass, method(pmClass, "get", String.class).invoke(pm, pkg), pkg, policy);
        review(pm, pmClass, apkClass, policy);
        System.out.println("PermissionRequestsTest: async grant/deny, result mapping and replacement race passed");
    }

    private static Object legacyPackage(Class<?> apkClass, String name, int uid) throws Exception {
        Class<?> parsedImpl = Class.forName("com.android.internal.pm.pkg.parsing.PackageImpl");
        Object parsed = parsedImpl.getMethod("forTesting", String.class, String.class)
                .invoke(null, name, "/fixture/" + name + "/base.apk");
        parsedImpl.getMethod("setEnabled", boolean.class).invoke(parsed, true);
        parsedImpl.getMethod("setTargetSdkVersion", int.class).invoke(parsed, 22);
        Class<?> usesClass = Class.forName("com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl");
        Constructor<?> usesCtor = usesClass.getConstructor(String.class, int.class, java.util.Set.class);
        Method addUses = parsedImpl.getMethod("addUsesPermission",
                Class.forName("com.android.internal.pm.pkg.component.ParsedUsesPermission"));
        addUses.invoke(parsed, usesCtor.newInstance(LOCATION, 0, Collections.emptySet()));
        addUses.invoke(parsed, usesCtor.newInstance(CAMERA, 0, Collections.emptySet()));
        Class<?> parsedPkg = Class.forName("com.android.internal.pm.pkg.parsing.ParsedPackage");
        Constructor<?> ctor = apkClass.getDeclaredConstructor(String.class, parsedPkg);
        ctor.setAccessible(true);
        Object apk = ctor.newInstance("/fixture/" + name + "/base.apk", parsed);
        java.lang.reflect.Field uidField = apkClass.getDeclaredField("mUid");
        uidField.setAccessible(true);
        uidField.setInt(apk, uid);
        return apk;
    }

    @SuppressWarnings("unchecked")
    private static void review(Object pm, Class<?> pmClass, Class<?> apkClass, Object policy) throws Exception {
        String legacyName = "fixture.permission.requests.legacy";
        Object legacy = legacyPackage(apkClass, legacyName, 19881);
        method(pmClass, "add", apkClass).invoke(pm, legacy);
        Method isReviewRequired = method(policy.getClass(), "isReviewRequired", apkClass);
        Method getFlags = policy.getClass().getMethod("getPermissionFlags", String.class, String.class,
                String.class, int.class);
        require((Boolean) isReviewRequired.invoke(policy, legacy),
                "legacy fixture did not start with REVIEW_REQUIRED");

        // 1. Cancel / dismiss and unavailable must NOT clear REVIEW_REQUIRED.
        for (int outcome : new int[]{DISMISSED, UNAVAILABLE}) {
            Object cancelled = controller(pm, pmClass, Runnable::run, (o, n) -> false,
                    (o, n) -> DENIED, (o, groups, enabled) -> {
                        require(groups.length == 2 && enabled[0] && enabled[1],
                                "review dialog did not default all groups to checked");
                        return outcome;
                    });
            CompletableFuture<Boolean> future = (CompletableFuture<Boolean>) method(cancelled.getClass(),
                    "reviewPermissions", apkClass).invoke(cancelled, legacy);
            require(!future.get() && (Boolean) isReviewRequired.invoke(policy, legacy),
                    "cancelled/unavailable review cleared REVIEW_REQUIRED or allowed launch");
        }

        // 2. Selective toggle + concurrent launch coalescing (single dialog for two launches).
        List<Runnable> queue = new ArrayList<>();
        AtomicInteger prompts = new AtomicInteger();
        Object selective = controller(pm, pmClass, queue::add, (o, n) -> false,
                (o, n) -> DENIED, (o, groups, enabled) -> {
                    prompts.incrementAndGet();
                    for (int i = 0; i < groups.length; i++) {
                        if (groups[i].contains("CAMERA")) enabled[i] = false;
                    }
                    return ALLOWED;
                });
        Method reviewMethod = method(selective.getClass(), "reviewPermissions", apkClass);
        CompletableFuture<Boolean> first = (CompletableFuture<Boolean>) reviewMethod.invoke(selective, legacy);
        CompletableFuture<Boolean> second = (CompletableFuture<Boolean>) reviewMethod.invoke(selective, legacy);
        require(first == second && queue.size() == 1,
                "concurrent launch attempts did not coalesce onto one review dialog");
        queue.remove(0).run();
        require(first.get() && second.get() && prompts.get() == 1,
                "coalesced review did not complete cleanly");
        require(!(Boolean) isReviewRequired.invoke(policy, legacy),
                "confirmed selective review left REVIEW_REQUIRED pending");
        int camFlags = (Integer) getFlags.invoke(policy, legacyName, CAMERA, "default:0", 0);
        int locFlags = (Integer) getFlags.invoke(policy, legacyName, LOCATION, "default:0", 0);
        require((camFlags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) != 0
                && (camFlags & PackageManager.FLAG_PERMISSION_USER_SET) != 0
                && (locFlags & PackageManager.FLAG_PERMISSION_REVOKED_COMPAT) == 0,
                "selective review did not soft-revoke only the toggled-off group");

        // 3. Confirm-all + replacement race on a fresh legacy package.
        String raceName = "fixture.permission.requests.legacy.race";
        Object raceApk = legacyPackage(apkClass, raceName, 19882);
        method(pmClass, "add", apkClass).invoke(pm, raceApk);
        Object replacement = legacyPackage(apkClass, raceName, 19882);
        Object stale = controller(pm, pmClass, Runnable::run, (o, n) -> false,
                (o, n) -> DENIED, (o, groups, enabled) -> {
                    try { method(pmClass, "add", apkClass).invoke(pm, replacement); }
                    catch (Exception e) { throw new AssertionError(e); }
                    return ALLOWED;
                });
        CompletableFuture<Boolean> staleFuture = (CompletableFuture<Boolean>) method(stale.getClass(),
                "reviewPermissions", apkClass).invoke(stale, raceApk);
        require(!staleFuture.get() && (Boolean) isReviewRequired.invoke(policy, replacement),
                "stale review cleared REVIEW_REQUIRED on replacement package");

        Object confirmAll = controller(pm, pmClass, Runnable::run, (o, n) -> false,
                (o, n) -> DENIED, (o, groups, enabled) -> ALLOWED);
        CompletableFuture<Boolean> allFuture = (CompletableFuture<Boolean>) method(confirmAll.getClass(),
                "reviewPermissions", apkClass).invoke(confirmAll, replacement);
        require(allFuture.get() && !(Boolean) isReviewRequired.invoke(policy, replacement),
                "confirm-all review did not clear REVIEW_REQUIRED");
        method(policy.getClass(), "forgetPackage", String.class).invoke(policy, legacyName);
        method(pmClass, "remove", String.class).invoke(pm, legacyName);
        method(policy.getClass(), "forgetPackage", String.class).invoke(policy, raceName);
        method(pmClass, "remove", String.class).invoke(pm, raceName);
        System.out.println("PermissionRequestsTest: legacy permission review passed");
    }
}
