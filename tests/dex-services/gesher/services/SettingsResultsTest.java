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
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Predicate;

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
    private static Object results(ScheduledExecutorService timer, long timeoutMs, Predicate<Object> alive) throws Exception {
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
                ScheduledExecutorService.class, long.class, long.class, Predicate.class);
        c.setAccessible(true);
        return c.newInstance(d, timer, timeoutMs, 25L, alive);
    }
    private static int delivered() { synchronized (DELIVERED) { return DELIVERED.size(); } }
    private static int[] last() { synchronized (DELIVERED) { return DELIVERED.get(DELIVERED.size() - 1); } }

    public static void main(String[] args) throws Exception {
        ScheduledThreadPoolExecutor timer = new ScheduledThreadPoolExecutor(1);
        timer.setRemoveOnCancelPolicy(true);
        try {
            var panelConstructor = type("SettingsResults$Panel").getDeclaredConstructor(int.class, long.class);
            panelConstructor.setAccessible(true);
            Object panel = panelConstructor.newInstance(42, 100L);
            Object reusedPid = panelConstructor.newInstance(42, 200L);
            Object results = results(timer, 60_000, unused -> true);
            String install = "android.settings.MANAGE_UNKNOWN_APP_SOURCES";
            String files = "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";

            // Nothing is answered until the Control Center reports the request's token.
            var coverReleases = new AtomicInteger();
            Field releaseCover = type("SettingsResults$Pending").getDeclaredField("releaseCover");
            releaseCover.setAccessible(true);
            Object first = pending("activity-a", 1, install, "app.install");
            releaseCover.set(first, (Runnable)coverReleases::incrementAndGet);
            String a = (String)call(results, "register", first);
            check(a.matches("[0-9a-f]{32}"), "A token is 32 hex digits: " + a);
            check(delivered() == 0, "A registered request is not answered at once");
            check((Integer)call(results, "pendingCount") == 1, "The request waits");
            check(coverReleases.get() == 0, "The activity stays covered while the request waits");
            // The user turned Install unknown apps on during the visit: RESULT_OK (AOSP
            // ExternalSourcesDetails' setResult on a change). Other apps' and ops' turns do not count.
            String unshown = (String)call(results, "register", pending("activity-unshown", 44, install, "app.install"));
            check((Integer)call(results, "shown", panel, new String[]{a}) == 1, "The real panel binds a visible visit");
            call(results, "toggled", panel, new String[]{a}, "app.other", OP, true);
            call(results, "toggled", panel, new String[]{a}, "app.install", "android:write_settings", true);
            call(results, "toggled", panel, new String[]{a}, "app.install", OP, true);
            call(results, "toggled", reusedPid, new String[]{a}, "app.install", OP, false);
            call(results, "cancel", a);
            check((Integer)call(results, "closed", reusedPid, new String[]{a}) == 0,
                    "PID reuse cannot answer the visible visit");
            check((Integer)call(results, "closed", panel, new String[]{a}) == 1, "The report answers the request");
            check(delivered() == 1 && last()[0] == 1 && last()[1] == Activity.RESULT_OK,
                    "Install unknown apps turned on during the visit answers RESULT_OK");
            check(a.equals(TOKENS.get(TOKENS.size() - 1)), "The answered request carries its token (its file goes)");
            check((Integer)call(results, "closed", panel, new String[]{a}) == 0 && delivered() == 1,
                    "A token is answered once");
            check(coverReleases.get() == 1, "Closing the screen releases its activity once");
            call(results, "closed", panel, new String[]{unshown});
            check(delivered() == 2 && last()[1] == Activity.RESULT_CANCELED,
                    "An unshown visit for the same package cannot inherit another visit's switch");

            // No turn (an app already allowed, closed as is), and other screens: RESULT_CANCELED.
            String b = (String)call(results, "register", pending("activity-b", 2, install, "app.install"));
            String c = (String)call(results, "register", pending("activity-c", 3, files, "app.install"));
            call(results, "toggled", panel, new String[]{b, c}, "app.install", OP, true);
            call(results, "toggled", panel, new String[]{b, c}, "app.install", OP, false);
            check(!b.equals(c), "Tokens differ");
            check((Integer)call(results, "closed", panel, new String[]{c, "not-a-token", null,
                    "ABCDEF0123456789ABCDEF0123456789"}) == 1, "Only valid, known tokens are answered");
            check(last()[0] == 3 && last()[1] == Activity.RESULT_CANCELED,
                    "Screens whose AOSP details set no result answer RESULT_CANCELED");
            call(results, "closed", panel, new String[]{b});
            check(last()[0] == 2 && last()[1] == Activity.RESULT_CANCELED,
                    "Install unknown apps last turned off answers RESULT_CANCELED");
            String e = (String)call(results, "register", pending("activity-e", 5, install, "app.install"));
            call(results, "closed", panel, new String[]{e});
            check(last()[0] == 5 && last()[1] == Activity.RESULT_CANCELED,
                    "A visit without a turn answers RESULT_CANCELED, even for an app already allowed");
            check((Integer)call(results, "closed", panel, null) == 0, "A null report answers nothing");

            // A request the Control Center never saw (it did not open) is dropped, not answered.
            Object abandoned = pending("activity-d", 4, files, "app.other");
            releaseCover.set(abandoned, (Runnable)coverReleases::incrementAndGet);
            String d = (String)call(results, "register", abandoned);
            call(results, "discard", d);
            check((Integer)call(results, "pendingCount") == 0 && delivered() == 5, "A discarded request is never answered");
            check((Integer)call(results, "closed", panel, new String[]{d}) == 0, "A discarded token cannot be answered later");
            check(coverReleases.get() == 2, "A failed open also releases the activity once");

            // Bounded admission cannot force an existing visible visit to finish.
            int max = (Integer)field("MAX_PENDING");
            int before = delivered();
            List<String> tokens = new ArrayList<>();
            for (int i = 0; i < max; i++) tokens.add((String)call(results, "register", pending("bulk-" + i, 100 + i, files, "app.other")));
            call(results, "shown", panel, new String[]{tokens.get(0)});
            check(call(results, "register", pending("rejected", 200, files, "app.other")) == null,
                    "New attempts are rejected at capacity");
            check((Integer)call(results, "pendingCount") == max, "At most " + max + " requests wait");
            check(delivered() == before, "No previously admitted visit was force-closed");
            check((Integer)call(results, "closed", panel, tokens.toArray(new String[0])) == max, "All visits stay answerable");
            check((Integer)call(results, "pendingCount") == 0, "Nothing is left waiting");

            // A Control Center that never reports: the timeout answers.
            var panelAlive = new java.util.concurrent.atomic.AtomicBoolean(true);
            Object quick = results(timer, 150, unused -> panelAlive.get());
            before = delivered();
            call(quick, "register", pending("activity-t", 7, install, "app.install"));
            String visible = (String)call(quick, "register", pending("activity-visible", 8, install, "app.install"));
            call(quick, "toggled", panel, new String[]{visible}, "app.install", OP, true);
            synchronized (DELIVERED) {
                long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (DELIVERED.size() == before && System.nanoTime() < end) DELIVERED.wait(100);
            }
            check(delivered() == before + 1 && last()[0] == 7 && last()[1] == Activity.RESULT_CANCELED,
                    "An unshown request expires");
            check((Integer)call(quick, "pendingCount") == 1, "A live visible visit survives the opening timeout");
            panelAlive.set(false);
            synchronized (DELIVERED) {
                long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (DELIVERED.size() <= before + 1 && System.nanoTime() < end) DELIVERED.wait(100);
            }
            check(delivered() == before + 2 && last()[0] == 8 && last()[1] == Activity.RESULT_CANCELED,
                    "The host process ending cancels its visit after a successful switch");
            check((Integer)call(quick, "pendingCount") == 0, "No retired visit remains");
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
