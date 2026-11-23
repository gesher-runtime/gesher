/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.Activity;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * The production SettingsResults: a per-app Settings screen opened for a result is answered
 * when the Control Center reports its token, with the result computed then; never twice,
 * never for a discarded request, at the latest after the timeout, and bounded. Delivery and
 * the install policy are the explicit boundaries (ActivityTaskManagerService supplies them).
 */
public final class SettingsResultsTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static final List<int[]> DELIVERED = new ArrayList<>();
    private static final List<Object> SOURCES = new ArrayList<>();
    private static final List<String> TOKENS = new ArrayList<>();
    private static final String OP = "android:request_install_packages";

    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object call(Object target, String name, Object... args) throws Exception {
        for (Method m : target.getClass().getDeclaredMethods()) {
            if (m.getName().equals(name) && m.getParameterCount() == args.length) {
                m.setAccessible(true);
                try { return m.invoke(target, args); }
                catch (InvocationTargetException e) { throw (Exception)e.getCause(); }
            }
        }
        throw new NoSuchMethodException(name);
    }
    private static Object pending(Object source, int requestCode, String action, String pkg) throws Exception {
        Constructor<?> c = type("SettingsResults$Pending").getDeclaredConstructor(Object.class, Object.class,
                String.class, int.class, String.class, String.class);
        c.setAccessible(true);
        return c.newInstance(source, "process", "who", requestCode, action, pkg);
    }
    private static Object results(ScheduledExecutorService timer, long timeoutMs) throws Exception {
        Class<?> delivery = type("SettingsResults$Delivery");
        InvocationHandler deliver = (proxy, method, args) -> {
            Field request = args[0].getClass().getDeclaredField("requestCode");
            request.setAccessible(true);
            Field source = args[0].getClass().getDeclaredField("source");
            source.setAccessible(true);
            Field token = args[0].getClass().getDeclaredField("token");
            token.setAccessible(true);
            synchronized (DELIVERED) {
                TOKENS.add((String)token.get(args[0]));
                DELIVERED.add(new int[] {request.getInt(args[0]), (Integer)args[1]});
                SOURCES.add(source.get(args[0]));
                DELIVERED.notifyAll();
            }
            return null;
        };
        Object d = Proxy.newProxyInstance(delivery.getClassLoader(), new Class<?>[]{delivery}, deliver);
        Constructor<?> c = type("SettingsResults").getDeclaredConstructor(delivery,
                ScheduledExecutorService.class, long.class);
        c.setAccessible(true);
        return c.newInstance(d, timer, timeoutMs);
    }
    private static int delivered() { synchronized (DELIVERED) { return DELIVERED.size(); } }
    private static int[] last() { synchronized (DELIVERED) { return DELIVERED.get(DELIVERED.size() - 1); } }

    public static void main(String[] args) throws Exception {
        ScheduledThreadPoolExecutor timer = new ScheduledThreadPoolExecutor(1);
        timer.setRemoveOnCancelPolicy(true);
        try {
            Object results = results(timer, 60_000);
            String install = "android.settings.MANAGE_UNKNOWN_APP_SOURCES";
            String files = "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";

            // Nothing is answered until the Control Center reports the request's token.
            String a = (String)call(results, "register", pending("activity-a", 1, install, "app.install"));
            check(a.matches("[0-9a-f]{32}"), "A token is 32 hex digits: " + a);
            check(delivered() == 0, "A registered request is not answered at once");
            check((Integer)call(results, "pendingCount") == 1, "The request waits");
            // The user turned Install unknown apps on during the visit: RESULT_OK (AOSP
            // ExternalSourcesDetails' setResult on a change). Other apps' and ops' turns do not count.
            call(results, "toggled", "app.other", OP, true);
            call(results, "toggled", "app.install", "android:write_settings", true);
            call(results, "toggled", "app.install", OP, true);
            check((Integer)call(results, "closed", (Object)new String[]{a}) == 1, "The report answers the request");
            check(delivered() == 1 && last()[0] == 1 && last()[1] == Activity.RESULT_OK,
                    "Install unknown apps turned on during the visit answers RESULT_OK");
            check(a.equals(TOKENS.get(TOKENS.size() - 1)), "The answered request carries its token (its file goes)");
            check((Integer)call(results, "closed", (Object)new String[]{a}) == 0 && delivered() == 1,
                    "A token is answered once");

            // No turn (an app already allowed, closed as is), and other screens: RESULT_CANCELED.
            String b = (String)call(results, "register", pending("activity-b", 2, install, "app.install"));
            String c = (String)call(results, "register", pending("activity-c", 3, files, "app.install"));
            call(results, "toggled", "app.install", OP, true);
            call(results, "toggled", "app.install", OP, false);
            check(!b.equals(c), "Tokens differ");
            check((Integer)call(results, "closed", (Object)new String[]{c, "not-a-token", null,
                    "ABCDEF0123456789ABCDEF0123456789"}) == 1, "Only valid, known tokens are answered");
            check(last()[0] == 3 && last()[1] == Activity.RESULT_CANCELED,
                    "Screens whose AOSP details set no result answer RESULT_CANCELED");
            call(results, "closed", (Object)new String[]{b});
            check(last()[0] == 2 && last()[1] == Activity.RESULT_CANCELED,
                    "Install unknown apps last turned off answers RESULT_CANCELED");
            String e = (String)call(results, "register", pending("activity-e", 5, install, "app.install"));
            call(results, "closed", (Object)new String[]{e});
            check(last()[0] == 5 && last()[1] == Activity.RESULT_CANCELED,
                    "A visit without a turn answers RESULT_CANCELED, even for an app already allowed");
            check((Integer)call(results, "closed", (Object)null) == 0, "A null report answers nothing");

            // A request the Control Center never saw (it did not open) is dropped, not answered.
            String d = (String)call(results, "register", pending("activity-d", 4, files, "app.other"));
            call(results, "discard", d);
            check((Integer)call(results, "pendingCount") == 0 && delivered() == 4, "A discarded request is never answered");
            check((Integer)call(results, "closed", (Object)new String[]{d}) == 0, "A discarded token cannot be answered later");

            // Bounded: the oldest waiting request is answered when the limit is reached.
            int max = (Integer)field("MAX_PENDING");
            int before = delivered();
            List<String> tokens = new ArrayList<>();
            for (int i = 0; i < max + 1; i++) tokens.add((String)call(results, "register", pending("bulk-" + i, 100 + i, files, "app.other")));
            check((Integer)call(results, "pendingCount") == max, "At most " + max + " requests wait");
            check(delivered() == before + 1 && last()[0] == 100, "The oldest request was answered to make room");
            check((Integer)call(results, "closed", (Object)tokens.toArray(new String[0])) == max, "The rest stay answerable");
            check((Integer)call(results, "pendingCount") == 0, "Nothing is left waiting");

            // A Control Center that never reports: the timeout answers.
            Object quick = results(timer, 150);
            before = delivered();
            call(quick, "register", pending("activity-t", 7, install, "app.install"));
            synchronized (DELIVERED) {
                long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (DELIVERED.size() == before && System.nanoTime() < end) DELIVERED.wait(100);
            }
            check(delivered() == before + 1 && last()[0] == 7 && last()[1] == Activity.RESULT_CANCELED,
                    "An unreported request is answered at the timeout");
            check((Integer)call(quick, "pendingCount") == 0, "The timeout leaves nothing behind");
            Thread.sleep(300);
            check(delivered() == before + 1, "A request is never answered twice");
            // The production timeout stays bounded.
            long timeout = (Long)field("TIMEOUT_MS");
            check(timeout > 60_000 && timeout <= 60 * 60_000, "Production timeout is bounded: " + timeout);
        } finally {
            timer.shutdownNow();
        }
        System.out.println("SettingsResultsTest all passed: " + checks + " checks");
    }
    private static Object field(String name) throws Exception {
        Field f = type("SettingsResults").getDeclaredField(name);
        f.setAccessible(true);
        return f.get(null);
    }
}
