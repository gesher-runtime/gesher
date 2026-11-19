/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityManager;
import android.bluetooth.BluetoothAdapter;
import android.content.ComponentName;
import android.content.Intent;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.Collections;

/** Bluetooth adapter request intents resolve via ActivityTaskManagerService rather than throwing ANFE. */
public final class BluetoothRequestIntentTest {
    private static int checks;

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
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
        for (Method method : atmClass.getDeclaredMethods()) {
            if (method.getName().equals("start") && method.getParameterCount() == 8) {
                start = method;
                break;
            }
        }
        require(start != null, "activity starter 8-arg method missing");
        start.setAccessible(true);

        String[] actions = {
                BluetoothAdapter.ACTION_REQUEST_ENABLE,
                BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE,
                "android.bluetooth.adapter.action.REQUEST_DISABLE"
        };
        for (String action : actions) {
            Intent intent = new Intent(action);
            if (BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE.equals(action)) {
                intent.putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 300);
            }
            Object result = start.invoke(atm, null, intent, null, "gesher.services",
                    null, null, -1, null);
            require(Integer.valueOf(ActivityManager.START_SUCCESS).equals(result),
                    action + " was not handled: " + result);
        }

        Intent targeted = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)
                .setComponent(new ComponentName("other.application", "MissingActivity"));
        Object unhandled = start.invoke(atm, null, targeted, null, "gesher.services",
                null, null, -1, null);
        require(!Integer.valueOf(ActivityManager.START_SUCCESS).equals(unhandled),
                "component-targeted intent should not match Bluetooth request handler: " + unhandled);

        System.out.println("BluetoothRequestIntentTest all passed: " + checks);
    }
}
