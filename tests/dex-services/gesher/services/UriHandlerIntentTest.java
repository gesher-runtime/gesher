/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityManager;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;

/**
 * Windows' mailto/tel/sms/geo handlers as Android activities
 * (com.gesher.server.WindowsUriHandlers), through the production PackageManagerService
 * and ActivityTaskManagerService and the real Windows association of this machine
 * (WindowsHost.hasUrlHandler: AssocQueryStringW). For each scheme: resolveIntent finds the
 * handler activity exactly when Windows has a handler; starting the intent hands Windows
 * the mapped URI then, and is START_INTENT_NOT_RESOLVED (ActivityNotFoundException) otherwise.
 * The external opener and DIAL confirmation/shell backend are isolated boundaries: nothing
 * opens. DIAL queues its work and opens only after the controlled confirmation accepts;
 * a second request declines confirmation. Real Windows dialog behavior is tested separately.
 * Prints "UriHandlerIntentTest all passed".
 */
public final class UriHandlerIntentTest {
    private static final String PACKAGE = "com.gesher.urihandlers";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static final class DialCapture implements AutoCloseable {
        final ArrayDeque<Runnable> pending = new ArrayDeque<>();
        final List<String> events = new ArrayList<>();
        final Object service;
        final Field field;
        final Object previous;
        boolean accept = true;

        DialCapture(Object atms, List<String> opened) throws Exception {
            service = atms;
            field = atms.getClass().getDeclaredField("mDialRequests");
            field.setAccessible(true);
            previous = field.get(atms);
            Class<?> requests = field.getType();
            Class<?> backend = Class.forName(requests.getName() + "$Backend");
            Object boundary = Proxy.newProxyInstance(backend.getClassLoader(), new Class<?>[] { backend },
                    (proxy, method, args) -> {
                        if ("confirm".equals(method.getName())) {
                            require(((BooleanSupplier) args[3]).getAsBoolean(), "DIAL confirmation is live");
                            events.add("confirm:" + args[1]);
                            return accept;
                        }
                        if ("open".equals(method.getName())) {
                            events.add("open:" + args[0]);
                            return opened.add((String) args[0]);
                        }
                        throw new AssertionError("Unexpected DIAL backend method: " + method);
                    });
            Constructor<?> constructor = requests.getDeclaredConstructor(Executor.class, backend);
            constructor.setAccessible(true);
            field.set(atms, constructor.newInstance((Executor) pending::add, boundary));
        }

        void drain() {
            require(pending.size() == 1, "exactly one DIAL request is queued");
            pending.remove().run();
            require(pending.isEmpty(), "DIAL request leaves no queued work");
        }

        @Override public void close() throws Exception {
            try {
                // An assertion can fail before drain(): cancel any remaining controlled
                // work so its production completion/lifecycle still runs.
                accept = false;
                while (!pending.isEmpty()) pending.remove().run();
            } finally {
                field.set(service, previous);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> host = Class.forName("com.gesher.server.WindowsHost");
        Method hasHandler = host.getMethod("hasUrlHandler", String.class);
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Constructor<?> pmConstructor = pmClass.getDeclaredConstructor(java.util.Collection.class);
        pmConstructor.setAccessible(true);
        Object pm = pmConstructor.newInstance(Collections.emptyList());
        Method resolve = pmClass.getMethod("resolveIntent", Intent.class, String.class, long.class, int.class);
        Method packageInfo = pmClass.getMethod("getPackageInfo", String.class, long.class, int.class);

        Class<?> atmsClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Constructor<?> atmsConstructor = atmsClass.getDeclaredConstructor(pmClass);
        atmsConstructor.setAccessible(true);
        Object atms = atmsConstructor.newInstance(pm);
        List<String> opened = Collections.synchronizedList(new ArrayList<>());
        Field opener = atmsClass.getDeclaredField("sOpenExternalUrl");
        opener.setAccessible(true);
        Object previousOpener = opener.get(null);
        opener.set(null, (Predicate<String>) url -> opened.add(url));
        try (DialCapture dial = new DialCapture(atms, opened)) {
            checkCases(hasHandler, pm, resolve, packageInfo, atmsClass, atms, opened, dial);
        } finally {
            opener.set(null, previousOpener);
        }
        System.out.println("UriHandlerIntentTest all passed");
        System.out.flush();
    }

    private static void checkCases(Method hasHandler, Object pm, Method resolve,
            Method packageInfo, Class<?> atmsClass, Object atms, List<String> opened,
            DialCapture capturedDial) throws Exception {
        Method start = null;
        for (Method m : atmsClass.getDeclaredMethods()) {
            if (m.getName().equals("start") && m.getParameterCount() == 8) start = m;
        }
        require(start != null, "no ActivityTaskManagerService.start(8 arguments)");
        start.setAccessible(true);

        Intent mail = new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:a@example.com"))
                .putExtra(Intent.EXTRA_SUBJECT, "Gesher test");
        Intent dial = new Intent(Intent.ACTION_DIAL, Uri.parse("tel:+15550100"));
        Intent sms = new Intent(Intent.ACTION_SENDTO, Uri.parse("smsto:+15550100"))
                .putExtra("sms_body", "Gesher test");
        Intent geo = new Intent(Intent.ACTION_VIEW, Uri.parse("geo:32.08,34.78?q=Tel%20Aviv"));
        Object[][] cases = {
            { "mailto", mail, "mailto:a@example.com?subject=Gesher%20test" },
            { "tel", dial, "tel:+15550100" },
            { "sms", sms, "sms:+15550100?body=Gesher%20test" },
            { "geo", geo, "geo:32.08,34.78?q=Tel%20Aviv" },
        };
        int registeredCount = 0;
        for (Object[] c : cases) {
            String scheme = (String) c[0];
            Intent intent = (Intent) c[1];
            String expected = (String) c[2];
            boolean registered = (Boolean) hasHandler.invoke(null, scheme);
            if (registered) registeredCount++;
            ResolveInfo ri = (ResolveInfo) resolve.invoke(pm, intent, null,
                    (long) PackageManager.MATCH_DEFAULT_ONLY, 0);
            require(registered == (ri != null && PACKAGE.equals(ri.activityInfo.packageName)),
                    scheme + ": Windows handler " + registered + " but resolveIntent " + ri);
            int before = opened.size();
            capturedDial.events.clear();
            Object result = start.invoke(atms, null, intent, null, "gesher.services", null, null, -1, null);
            if (registered) {
                require(Integer.valueOf(ActivityManager.START_SUCCESS).equals(result),
                        scheme + ": start returned " + result);
                if (Intent.ACTION_DIAL.equals(intent.getAction())) {
                    require(opened.size() == before && capturedDial.events.isEmpty(),
                            "DIAL must return before confirmation or shell work");
                    capturedDial.drain();
                    require(capturedDial.events.equals(List.of("confirm:+15550100", "open:" + expected)),
                            "DIAL must confirm the exact displayed number before opening its URI: "
                                    + capturedDial.events);
                }
                require(opened.size() == before + 1 && expected.equals(opened.get(before)),
                        scheme + ": Windows was asked to open " + opened);
                if (Intent.ACTION_DIAL.equals(intent.getAction())) {
                    capturedDial.accept = false;
                    capturedDial.events.clear();
                    Object canceled = start.invoke(atms, null, intent, null, "gesher.services", null, null, -1, null);
                    require(Integer.valueOf(ActivityManager.START_SUCCESS).equals(canceled),
                            "DIAL cancellation still starts its confirmation activity");
                    require(opened.size() == before + 1 && capturedDial.events.isEmpty(),
                            "DIAL cancellation is also queued");
                    capturedDial.drain();
                    require(capturedDial.events.equals(List.of("confirm:+15550100"))
                                    && opened.size() == before + 1,
                            "Cancel must not open the URI: " + capturedDial.events);
                    capturedDial.accept = true;
                    System.out.println("UriHandlerIntentTest: ACTION_DIAL queues exact-number confirmation; "
                            + "controlled Accept opens once and Cancel never opens (UI/shell boundary fixture)");
                }
            } else {
                require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(result),
                        scheme + ": start without a Windows handler returned " + result);
                require(opened.size() == before, scheme + ": opened without a Windows handler " + opened);
                require(capturedDial.pending.isEmpty() && capturedDial.events.isEmpty(),
                        scheme + ": unregistered handler queued confirmation work");
            }
            System.out.println("UriHandlerIntentTest: " + scheme + " Windows handler "
                    + (registered ? "registered: resolves and opens " + expected
                            : "not registered: no activity, START_INTENT_NOT_RESOLVED"));
        }
        PackageInfo pi = (PackageInfo) packageInfo.invoke(pm, PACKAGE, (long) PackageManager.GET_ACTIVITIES, 0);
        require((registeredCount > 0) == (pi != null), "getPackageInfo(" + PACKAGE + ") " + pi
                + " with " + registeredCount + " Windows handlers");
        require(pi == null || pi.activities.length == registeredCount,
                "getPackageInfo lists " + (pi == null ? 0 : pi.activities.length) + " activities for "
                + registeredCount + " Windows handlers");

        // Never a handler: ACTION_CALL (no confirmation), mms, SEND message/rfc822.
        Intent[] never = {
            new Intent(Intent.ACTION_CALL, Uri.parse("tel:+15550100")),
            new Intent(Intent.ACTION_SENDTO, Uri.parse("mmsto:+15550100")),
            new Intent(Intent.ACTION_SEND).setType("message/rfc822"),
        };
        for (Intent intent : never) {
            require(resolve.invoke(pm, intent, null, (long) PackageManager.MATCH_DEFAULT_ONLY, 0) == null,
                    "resolves: " + intent);
        }
        System.out.println("UriHandlerIntentTest: CALL, mms and SEND message/rfc822 have no Windows handler activity");
        require(capturedDial.pending.isEmpty(), "URI cases leave no queued DIAL work");
    }
}
