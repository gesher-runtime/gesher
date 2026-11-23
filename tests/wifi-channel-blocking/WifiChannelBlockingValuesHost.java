/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** Host-only value behavior; framework interfaces are genuine, Parcel is never executed. */
public final class WifiChannelBlockingValuesHost {
    private static final String PREFIX = "android.net.wifi.";
    private static final String[] TYPES = {"WifiAvailableChannel", "BlockingOption", "BlockingOption$Builder"};
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean owned = name.startsWith("gesher.services.WifiChannelBlockingValuesContractTest");
            for (String type : TYPES) owned |= name.equals(PREFIX + type) || name.startsWith(PREFIX + type + "$");
            if (!owned) return super.loadClass(name, resolve);
            Class<?> result = findLoadedClass(name);
            if (result == null) result = findClass(name); // exact absence must not fall back to SDK stubs
            if (resolve) resolveClass(result);
            return result;
        }
    }
    private static void provider(String name, Path expected) throws Exception {
        Class<?> type = Class.forName(name, false, WifiChannelBlockingValuesHost.class.getClassLoader());
        Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        if (!actual.equals(expected)) throw new AssertionError("Unexpected provider for " + name + ": " + actual);
        System.out.println("WIFI_CHANNEL_BLOCKING_HOST_PROVIDER " + name + "=" + actual);
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 4) throw new IllegalArgumentException("classes callers framework candidate|pinned-r4|baseline");
        Path classes = Path.of(args[0]).toRealPath();
        if (args[3].equals("baseline")) {
            try (BaselineLoader loader = new BaselineLoader(new URL[] {classes.toUri().toURL(),
                    Path.of(args[1]).toRealPath().toUri().toURL()}, WifiChannelBlockingValuesHost.class.getClassLoader())) {
                Class.forName("gesher.services.WifiChannelBlockingValuesContractTest", true, loader)
                        .getMethod("baselineControl").invoke(null);
            }
            return;
        }
        for (String type : TYPES) provider(PREFIX + type, classes);
        for (String type : new String[] {"WifiAvailableChannel$1", "WifiAvailableChannel$Filter",
                "WifiAvailableChannel$OpMode", "BlockingOption$1"}) provider(PREFIX + type, classes);
        for (String name : new String[] {"android.os.Parcel", "android.os.Parcelable"})
            provider(name, Path.of(args[2]).toRealPath());
        WifiChannelBlockingValuesContractTest.run(false);
        Class.forName("gesher.services.WifiChannelBlockingValuesExtendedTest")
                .getMethod("run", boolean.class).invoke(null, false);
        WifiChannelBlockingValuesContractTest.finish(false);
    }
}
