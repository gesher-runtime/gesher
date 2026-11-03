/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.net.Uri;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.function.Predicate;

/**
 * The Windows default browser as a package (com.gesher.server.BrowserPackageSupport): an
 * app that looks for a browser as AppAuth's BrowserSelector does (queryIntentActivities of
 * VIEW http BROWSABLE with GET_RESOLVED_FILTER, its filter a full browser's, then
 * getPackageInfo with GET_SIGNATURES) finds com.gesher.browser, and a start of an http(s)
 * VIEW for it (setPackage, its component, or implicit) goes to Windows
 * (ActivityTaskManagerService.sOpenExternalUrl, replaced here so that no browser opens).
 * Prints "BrowserIntentTest all passed".
 */
public final class BrowserIntentTest {
    private static final String BROWSER = "com.gesher.browser";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Constructor<?> pmConstructor = pmClass.getDeclaredConstructor(java.util.Collection.class);
        pmConstructor.setAccessible(true);
        Object pm = pmConstructor.newInstance(Collections.emptyList());

        // BrowserSelector.BROWSER_INTENT, with its flags.
        Intent browserIntent = new Intent(Intent.ACTION_VIEW, Uri.fromParts("http", "", null))
                .addCategory(Intent.CATEGORY_BROWSABLE);
        Method query = pmClass.getMethod("queryIntentActivities", Intent.class, String.class,
                long.class, int.class);
        List<ResolveInfo> found = ((ParceledListSlice<ResolveInfo>) query.invoke(pm,
                browserIntent, null, (long) (PackageManager.GET_RESOLVED_FILTER
                        | PackageManager.MATCH_ALL), 0)).getList();
        ResolveInfo browser = null;
        for (ResolveInfo ri : found) {
            if (ri.activityInfo != null && BROWSER.equals(ri.activityInfo.packageName)) {
                browser = ri;
            }
        }
        require(browser != null, "no " + BROWSER + " among " + found.size() + " activities");
        IntentFilter filter = browser.filter;
        require(filter != null, "no resolved filter (GET_RESOLVED_FILTER)");
        require(filter.hasAction(Intent.ACTION_VIEW) && filter.hasCategory(Intent.CATEGORY_BROWSABLE)
                && filter.hasCategory(Intent.CATEGORY_DEFAULT), "the filter is not VIEW, BROWSABLE, DEFAULT");
        List<String> schemes = new ArrayList<>();
        for (Iterator<String> it = filter.schemesIterator(); it != null && it.hasNext(); ) {
            schemes.add(it.next());
        }
        require(schemes.contains("http") && schemes.contains("https"),
                "the filter's schemes are " + schemes);
        require(filter.authoritiesIterator() == null, "the filter is limited to authorities");
        System.out.println("BrowserIntentTest: queryIntentActivities(VIEW http BROWSABLE) finds "
                + BROWSER + " (" + browser.activityInfo.name + "), a full browser's filter");

        Method resolve = pmClass.getMethod("resolveIntent", Intent.class, String.class,
                long.class, int.class);
        ResolveInfo resolved = (ResolveInfo) resolve.invoke(pm, browserIntent, null,
                (long) PackageManager.MATCH_DEFAULT_ONLY, 0);
        require(resolved != null && BROWSER.equals(resolved.activityInfo.packageName),
                "resolveActivity(VIEW http) is not the browser");
        Intent custom = new Intent(Intent.ACTION_VIEW, Uri.parse("myapp://callback"))
                .addCategory(Intent.CATEGORY_BROWSABLE);
        require(((ParceledListSlice<ResolveInfo>) query.invoke(pm, custom, null, 0L, 0))
                .getList().isEmpty(), "a custom scheme resolves to something");

        Method packageInfo = pmClass.getMethod("getPackageInfo", String.class, long.class,
                int.class);
        PackageInfo pi = (PackageInfo) packageInfo.invoke(pm, BROWSER,
                (long) PackageManager.GET_SIGNATURES, 0);
        require(pi != null && pi.signatures != null && pi.signatures.length == 1
                && pi.signatures[0].toByteArray().length > 0, "no signature of " + BROWSER);
        require(pi.versionName != null, "no versionName");
        Method services = pmClass.getMethod("queryIntentServices", Intent.class, String.class,
                long.class, int.class);
        Intent customTabs = new Intent("android.support.customtabs.action.CustomTabsService")
                .setPackage(BROWSER);
        require(((ParceledListSlice<ResolveInfo>) services.invoke(pm, customTabs, null, 0L, 0))
                .getList().isEmpty(), BROWSER + " has a custom tabs service");
        System.out.println("BrowserIntentTest: getPackageInfo(" + BROWSER
                + ", GET_SIGNATURES) has its signature; no custom tabs service");

        // Starts: each http(s) VIEW for the browser goes to Windows, nothing is recorded.
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
        Intent[] intents = {
            new Intent(Intent.ACTION_VIEW, Uri.parse("https://accounts.example.com/auth?a=1&b=2"))
                    .addCategory(Intent.CATEGORY_BROWSABLE).setPackage(BROWSER),
            new Intent(Intent.ACTION_VIEW, Uri.parse("http://www.example.com/component"))
                    .setComponent(new ComponentName(BROWSER, BROWSER + ".WindowsBrowserActivity")),
            new Intent(Intent.ACTION_VIEW, Uri.parse("https://www.example.com/implicit")),
        };
        for (Intent intent : intents) {
            // No activity is recorded: started stays untouched (null, as DocumentPickerTest).
            Object result = start.invoke(atms, null, intent, null, "gesher.services", null, null,
                    -1, null);
            require(Integer.valueOf(0).equals(result),
                    "start(" + intent + ") returned " + result + ", not START_SUCCESS");
        }
        require(opened.size() == 3 && opened.get(0).equals("https://accounts.example.com/auth?a=1&b=2")
                && opened.get(1).equals("http://www.example.com/component")
                && opened.get(2).equals("https://www.example.com/implicit"),
                "Windows was asked to open " + opened);
        Object none = start.invoke(atms, null, custom, null, "gesher.services", null, null, -1,
                null);
        require(!Integer.valueOf(0).equals(none) && opened.size() == 3,
                "a custom scheme no app handles went somewhere (" + none + ")");
        System.out.println("BrowserIntentTest: starting VIEW http(s) for " + BROWSER
                + " (package, component, implicit) delegates the URL to Windows");
        System.out.println("BrowserIntentTest all passed");
        System.out.flush();
    }
}
