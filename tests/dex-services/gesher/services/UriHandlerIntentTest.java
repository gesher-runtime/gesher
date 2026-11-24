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
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.function.Predicate;

/**
 * Windows' mailto/tel/sms/geo handlers as Android activities
 * (com.gesher.server.WindowsUriHandlers), through the production PackageManagerService
 * and ActivityTaskManagerService and the real Windows association of this machine
 * (WindowsHost.hasUrlHandler: AssocQueryStringW). For each scheme: resolveIntent finds the
 * handler activity exactly when Windows has a handler; starting the intent hands Windows
 * the mapped URI then (ActivityTaskManagerService.sOpenExternalUrl, replaced here so that
 * nothing opens), and is START_INTENT_NOT_RESOLVED (ActivityNotFoundException) otherwise.
 * Prints "UriHandlerIntentTest all passed".
 */
public final class UriHandlerIntentTest {
    private static final String PACKAGE = "com.gesher.urihandlers";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
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
        opener.set(null, (Predicate<String>) url -> opened.add(url));
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
            Object result = start.invoke(atms, null, intent, null, "gesher.services", null, null, -1, null);
            if (registered) {
                require(Integer.valueOf(ActivityManager.START_SUCCESS).equals(result),
                        scheme + ": start returned " + result);
                require(opened.size() == before + 1 && expected.equals(opened.get(before)),
                        scheme + ": Windows was asked to open " + opened);
            } else {
                require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(result),
                        scheme + ": start without a Windows handler returned " + result);
                require(opened.size() == before, scheme + ": opened without a Windows handler " + opened);
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
        System.out.println("UriHandlerIntentTest all passed");
        System.out.flush();
    }
}
