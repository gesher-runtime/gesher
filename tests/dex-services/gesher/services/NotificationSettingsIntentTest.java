/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityManager;
import android.app.WaitResult;
import android.content.ComponentName;
import android.content.Intent;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.function.Predicate;

/** Gmail's "Manage notifications": resolve through the activity starter, with no UI opened. */
public final class NotificationSettingsIntentTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Constructor<?> pmConstructor = pmClass.getDeclaredConstructor(java.util.Collection.class);
        pmConstructor.setAccessible(true);
        Object pm = pmConstructor.newInstance(Collections.emptyList());
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Constructor<?> constructor = atmClass.getDeclaredConstructor(pmClass);
        constructor.setAccessible(true);
        Object atm = constructor.newInstance(pm);
        Method start = null;
        Method waitStart = null;
        for (Method method : atmClass.getDeclaredMethods()) {
            if (method.getName().equals("start") && method.getParameterCount() == 8) start = method;
            if (method.getName().equals("startActivityAndWait")) waitStart = method;
        }
        require(start != null && waitStart != null, "activity starter methods missing");
        start.setAccessible(true);
        Field opener = atmClass.getDeclaredField("sOpenNotificationSettings");
        opener.setAccessible(true);
        Object previous = opener.get(null);
        List<String> opened = new ArrayList<>();
        try {
            opener.set(null, (Predicate<String>) uri -> opened.add(uri));
            String[] actions = {"android.settings.APP_NOTIFICATION_SETTINGS",
                    "android.settings.CHANNEL_NOTIFICATION_SETTINGS"};
            for (String action : actions) {
                Intent intent = new Intent(action)
                        .putExtra("android.provider.extra.APP_PACKAGE", "com.google.android.gm")
                        .putExtra("android.provider.extra.CHANNEL_ID", "arbitrary:/channel?&value");
                Object result = start.invoke(atm, null, intent, null, "gesher.services", null,
                        null, -1, null);
                require(Integer.valueOf(ActivityManager.START_SUCCESS).equals(result),
                        action + " was not handled: " + result);
            }
            require(opened.equals(java.util.Arrays.asList("ms-settings:notifications",
                    "ms-settings:notifications")), "unexpected Windows URIs: " + opened);

            Intent channel = new Intent(actions[1]).setPackage("com.android.settings");
            WaitResult result = (WaitResult) waitStart.invoke(atm, null, "gesher.services", null,
                    channel, null, null, null, -1, 0, null, null, 0);
            require(result.result == ActivityManager.START_SUCCESS && !result.timeout,
                    "startActivityAndWait did not finish successfully");
            require(opened.size() == 3, "package-targeted Settings launch was not delegated");

            Intent[] unhandled = {
                    new Intent(actions[1]).setPackage("other.application"),
                    new Intent(actions[1]).setComponent(new ComponentName("other.application", "Missing")),
                    new Intent("android.settings.NOT_A_SUPPORTED_NOTIFICATION_ACTION"),
                    new Intent("android.settings.SYNC_SETTINGS"),
            };
            for (Intent intent : unhandled) {
                Object status = start.invoke(atm, null, intent, null, "gesher.services", null,
                        null, -1, null);
                require(!Integer.valueOf(ActivityManager.START_SUCCESS).equals(status),
                        "unsupported intent was swallowed: " + intent);
            }
            require(opened.size() == 3, "unsupported intent opened Windows Settings");
            opener.set(null, (Predicate<String>) uri -> false);
            Object failed = start.invoke(atm, null, new Intent(actions[0]), null,
                    "gesher.services", null, null, -1, null);
            require(Integer.valueOf(ActivityManager.START_INTENT_NOT_RESOLVED).equals(failed),
                    "failed Windows launch reported success");
        } finally {
            opener.set(null, previous);
        }
        System.out.println("NotificationSettingsIntentTest all passed");
    }
}
