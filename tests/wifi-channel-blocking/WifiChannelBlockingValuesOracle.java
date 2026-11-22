/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import dalvik.system.PathClassLoader;
import java.lang.reflect.InvocationTargetException;

/** Normal records through complete source-owned values and genuine boot Parcel leaves. */
public final class WifiChannelBlockingValuesOracle {
    private static final String PREFIX = "android.net.wifi.";
    private static final String[] TYPES = {"WifiAvailableChannel", "BlockingOption", "BlockingOption$Builder"};
    private static final class OwnedLoader extends PathClassLoader {
        OwnedLoader(String path, ClassLoader parent) { super(path, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean owned = name.startsWith("gesher.services.WifiChannelBlockingValues");
            for (String type : TYPES) owned |= name.equals(PREFIX + type)
                    || name.startsWith(PREFIX + type + "$") || name.equals(PREFIX + type + "-IA");
            if (!owned) return super.loadClass(name, resolve);
            Class<?> value = findLoadedClass(name);
            if (value == null) value = findClass(name);
            if (resolve) resolveClass(value);
            return value;
        }
    }
    private static void require(boolean value, String label) { if (!value) throw new AssertionError(label); }
    private static Class<?> type(String name, ClassLoader loader) throws Exception { return Class.forName(name, true, loader); }
    private static void provider(String name, ClassLoader loader, ClassLoader expected) throws Exception {
        require(type(name, loader).getClassLoader() == expected, "Wrong provider: " + name);
        System.out.println("WIFI_CHANNEL_BLOCKING_ORACLE_PROVIDER " + name + "=" + expected);
    }
    private static void stockPresence(ClassLoader loader) throws Exception {
        boolean complete = true;
        for (String name : TYPES) {
            try {
                System.out.println("WIFI_CHANNEL_BLOCKING_STOCK_PRESENCE " + PREFIX + name + "=" + type(PREFIX + name, loader).getClassLoader());
            } catch (ClassNotFoundException missing) {
                complete = false;
                System.out.println("WIFI_CHANNEL_BLOCKING_STOCK_PRESENCE " + PREFIX + name + "=absent");
            }
        }
        require(complete, "Incomplete stock type profile cannot qualify the corpus");
        Class<?> channel = type(PREFIX + TYPES[0], loader), blocking = type(PREFIX + TYPES[1], loader);
        try {
            channel.getMethod("getChannelWidth");
            channel.getMethod("getUsableFilter");
            channel.getConstructor(int.class, int.class, int.class);
            blocking.getConstructor(int.class, boolean.class);
            blocking.getConstructor(Parcel.class);
        } catch (NoSuchMethodException missing) {
            System.out.println("WIFI_CHANNEL_BLOCKING_STOCK_MEMBER_ABSENT " + missing.getMessage());
            throw new AssertionError("Incomplete stock member profile cannot qualify the corpus", missing);
        }
    }
    private static void graph(ClassLoader loader, boolean stock) throws Exception {
        ClassLoader boot = Parcel.class.getClassLoader(), owner = stock ? boot : loader;
        require(boot == null || boot.getClass().getName().endsWith("BootClassLoader"), "Genuine boot Parcel required");
        for (Class<?> leaf : new Class<?>[] {Parcel.class, Parcelable.class, Build.class})
            require(type(leaf.getName(), loader) == leaf && leaf.getClassLoader() == boot, "Genuine boot leaf: " + leaf);
        for (String name : TYPES) {
            Class<?> value = type(PREFIX + name, loader);
            provider(value.getName(), loader, owner);
            require(value.getSuperclass() == Object.class, "Original Object ancestry");
            if (!name.endsWith("$Builder")) {
                require(Parcelable.class.isAssignableFrom(value), "Genuine Parcelable identity");
                require(value.getField("CREATOR").get(null).getClass().getClassLoader() == owner, "Original CREATOR provider");
            }
        }
        for (String name : new String[] {"WifiAvailableChannel$OpMode", "WifiAvailableChannel$Filter"})
            provider(PREFIX + name, loader, owner);
        for (String name : new String[] {"WifiChannelBlockingValuesContractTest", "WifiChannelBlockingValuesExtendedTest"})
            provider("gesher.services." + name, loader, loader);
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(0x24681357);
            require(parcel.marshall().length == 4, "Native primitive extent");
            parcel.setDataPosition(0);
            require(parcel.readInt() == 0x24681357 && parcel.dataAvail() == 0, "Native primitive full reader");
        } finally { parcel.recycle(); }
        System.out.println("WIFI_CHANNEL_BLOCKING_ORACLE_PROVIDERS graph=" + (stock ? "stock" : "owned")
                + "; genuine boot Parcel; complete original readers");
    }
    private static void invoke(Class<?> fixture, String method) throws Exception {
        try { fixture.getMethod(method, boolean.class).invoke(null, true); }
        catch (InvocationTargetException failure) { failure.getCause().printStackTrace(System.err); throw failure; }
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2 && (args[0].equals("stock") || args[0].equals("candidate") || args[0].equals("pinned-r4")),
                "stock|candidate|pinned-r4 <payload.jar>");
        require(Build.VERSION.SDK_INT == 36, "Exact API36 oracle required");
        boolean stock = args[0].equals("stock");
        ClassLoader parent = WifiChannelBlockingValuesOracle.class.getClassLoader();
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent) : new OwnedLoader(args[1], parent);
        System.out.println("WIFI_CHANNEL_BLOCKING_ORACLE_PROFILE mode=" + args[0] + "; sdk=" + Build.VERSION.SDK_INT
                + "; uid=" + android.os.Process.myUid() + "; vm=" + System.getProperty("java.vm.name")
                + "; fingerprint=" + Build.FINGERPRINT);
        if (stock) stockPresence(loader);
        graph(loader, stock);
        Class<?> publicFixture = type("gesher.services.WifiChannelBlockingValuesContractTest", loader);
        invoke(publicFixture, "run");
        invoke(type("gesher.services.WifiChannelBlockingValuesExtendedTest", loader), "run");
        invoke(publicFixture, "finish");
        System.out.println("WIFI_CHANNEL_BLOCKING_ORACLE_COMPLETE mode=" + args[0] + "; nativeParcel=true; fullReaders=16");
    }
}
