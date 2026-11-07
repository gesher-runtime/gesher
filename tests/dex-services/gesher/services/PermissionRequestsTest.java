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
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiPredicate;
import java.util.function.Consumer;

/** Server-local consent injection; no Windows security prompt is automated. */
public final class PermissionRequestsTest {
    private static final String LOCATION = "android.permission.ACCESS_COARSE_LOCATION";
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
        Class<?> requests = Class.forName("com.gesher.server.PermissionRequests");
        Class<?> choices = Class.forName("com.gesher.server.PermissionRequests$Decision");
        Object choice = Proxy.newProxyInstance(choices.getClassLoader(), new Class<?>[]{choices},
                (proxy, called, args) -> decision.test((String) args[0], (String) args[1]));
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
        policy.getClass().getMethod("revokeRuntimePermission", String.class, String.class,
                String.class, int.class, String.class)
                .invoke(policy, pkg, LOCATION, "default:0", 0, "regression test");
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
        System.out.println("PermissionRequestsTest: async grant/deny, result mapping and replacement race passed");
    }
}
