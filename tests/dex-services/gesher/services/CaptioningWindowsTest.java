/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import java.lang.reflect.Method;
import java.util.Map;

/**
 * Windows' caption style through the production natives on ART (WindowsCaptions, over
 * ClosedCaptionProperties) and its mapping to CaptioningManager's Settings.Secure keys
 * (CaptioningSettings). This machine's style is read, never changed.
 * Prints "CaptioningWindowsTest all passed".
 */
public final class CaptioningWindowsTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Class<?> windowsClass = Class.forName("com.gesher.server.WindowsCaptions");
        Method create = windowsClass.getDeclaredMethod("create");
        create.setAccessible(true);
        Object windows = create.invoke(null);
        require(windows != null, "WindowsCaptions.create: no caption style from Windows");
        Method read = windowsClass.getMethod("read");
        read.setAccessible(true);
        int[] values = (int[]) read.invoke(windows);
        require(values != null && values.length == 12, "read: " + (values == null ? null : values.length));
        Class<?> settings = Class.forName("com.gesher.server.CaptioningSettings");
        Method secureValues = settings.getDeclaredMethod("secureValues", int[].class);
        secureValues.setAccessible(true);
        Map<String, String> secure = (Map<String, String>) secureValues.invoke(null, (Object) values);
        require(secure != null, "Windows answered a value CaptioningSettings does not know: "
                + java.util.Arrays.toString(values));
        System.out.println("CaptioningWindowsTest: Windows caption style " + java.util.Arrays.toString(values)
                + " -> " + secure);
        System.out.println("CaptioningWindowsTest all passed");
        System.out.flush();
    }
}
