/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.lang.reflect.InvocationTargetException;
import java.util.Arrays;

/** Private writable cache region for isolated in-process server fixtures, not a device bootstrap. */
public final class ServiceFixtureRunner {
    public static void main(String[] args) throws Throwable {
        if (args.length == 0 || !args[0].matches("[A-Za-z][A-Za-z0-9]*")) {
            throw new IllegalArgumentException("Pass a service fixture class name");
        }
        // These wrapper-capability fixtures run Main.bootstrap under the production boot
        // loader and never mutate the package registry. Do not pre-load their JNI library
        // under this test loader. The account-cache fixture owns its own real region.
        boolean ownsBootstrap = args[0].equals("SdkSandboxUnsupportedTest")
                || args[0].equals("NetworkStatsUnavailableTest")
                || args[0].equals("WifiUnsupportedTest")
                || args[0].equals("WifiScannerUnsupportedTest")
                || args[0].equals("AccountVisibilityNotificationTest");
        if (!ownsBootstrap) {
            System.loadLibrary("android_runtime");
            Class<?> shared = Class.forName("com.android.internal.os.ApplicationSharedMemory");
            Object region = shared.getMethod("create").invoke(null);
            shared.getMethod("setInstance", shared).invoke(null, region);
        }
        try {
            Class.forName("gesher.services." + args[0]).getMethod("main", String[].class)
                    .invoke(null, (Object)Arrays.copyOfRange(args, 1, args.length));
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
}
