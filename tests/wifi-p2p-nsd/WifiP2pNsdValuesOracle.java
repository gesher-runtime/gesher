/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import dalvik.system.PathClassLoader;
import java.lang.reflect.InvocationTargetException;

/** Isolated complete value graphs, original readers and genuine Android boot Parcel. */
public final class WifiP2pNsdValuesOracle {
    private static final String PREFIX = "android.net.wifi.p2p.nsd.";
    private static final String[] LEGACY = {"WifiP2pServiceInfo", "WifiP2pServiceRequest",
            "WifiP2pDnsSdServiceInfo", "WifiP2pDnsSdServiceRequest", "WifiP2pUpnpServiceInfo", "WifiP2pUpnpServiceRequest"};
    private static final String[] CONFIG = {"WifiP2pUsdBasedServiceConfig", "WifiP2pUsdBasedServiceConfig$Builder"};
    private static final String HELPER = "com.android.net.module.util.DnsSdTxtRecord";
    private static final class OwnedLoader extends PathClassLoader {
        private final boolean pinned;
        OwnedLoader(String path, ClassLoader parent, boolean pinned) { super(path, parent); this.pinned = pinned; }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean owned = name.startsWith(PREFIX) || name.startsWith("gesher.services.WifiP2pNsdValues")
                    || name.equals(HELPER) || name.startsWith(HELPER + "$")
                    || (pinned && (name.equals("android.net.wifi.util.Environment") || name.startsWith("com.android.wifi.flags.")));
            if (!owned) return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name); // genuine selected graph, never stock/stub fallback
            if (resolve) resolveClass(type);
            return type;
        }
    }
    private static void require(boolean ok, String label) { if (!ok) throw new AssertionError(label); }
    private static Class<?> type(String name, ClassLoader loader) throws Exception { return Class.forName(name, true, loader); }
    private static void provider(String name, ClassLoader lookup, ClassLoader expected) throws Exception {
        require(type(name, lookup).getClassLoader() == expected, "Wrong provider: " + name);
        System.out.println("P2P_NSD_PROVIDER " + name + "=" + expected);
    }
    private static void graph(ClassLoader loader, boolean stock, boolean pinned) throws Exception {
        ClassLoader boot = Parcel.class.getClassLoader();
        require(boot == null || boot.getClass().getName().endsWith("BootClassLoader"), "actual boot Parcel required");
        require(type("android.os.Parcel", loader) == Parcel.class && type("android.os.Parcelable", loader) == Parcelable.class,
                "native Parcel/Parcelable must not be replaced");
        for (String name : LEGACY) provider(PREFIX + name, loader, stock ? boot : loader);
        for (String name : new String[] {"WifiP2pServiceInfo", "WifiP2pServiceRequest"}) {
            Object creator = type(PREFIX + name, loader).getField("CREATOR").get(null);
            require(creator.getClass().getClassLoader() == (stock ? boot : loader), "original base CREATOR provider");
        }
        Class<?> info = type(PREFIX + "WifiP2pServiceInfo", loader);
        Class<?> request = type(PREFIX + "WifiP2pServiceRequest", loader);
        for (String prefix : new String[] {"WifiP2pDnsSd", "WifiP2pUpnp"}) {
            require(type(PREFIX + prefix + "ServiceInfo", loader).getSuperclass() == info, "Info superclass identity");
            require(type(PREFIX + prefix + "ServiceRequest", loader).getSuperclass() == request, "Request superclass identity");
        }
        if (!stock) {
            for (String name : CONFIG) provider(PREFIX + name, loader, loader);
            provider(HELPER, loader, loader); provider(HELPER + "$1", loader, loader);
            Class<?> config = type(PREFIX + CONFIG[0], loader);
            require(config.getField("CREATOR").get(null).getClass().getClassLoader() == loader, "original Config CREATOR provider");
            require(info.getMethod("getWifiP2pUsdBasedServiceConfig").getReturnType() == config, "Info Config identity");
            require(request.getMethod("getWifiP2pUsdBasedServiceConfig").getReturnType() == config, "Request Config identity");
            require(info.getConstructor(config).getParameterTypes()[0] == config, "Info Config constructor identity");
            require(request.getConstructor(config).getParameterTypes()[0] == config, "Request Config constructor identity");
        }
        if (pinned) {
            for (String name : new String[] {"android.net.wifi.util.Environment", "com.android.wifi.flags.Flags",
                    "com.android.wifi.flags.FeatureFlags", "com.android.wifi.flags.FeatureFlagsImpl"}) provider(name, loader, loader);
            require(Boolean.TRUE.equals(type("com.android.wifi.flags.Flags", loader).getMethod("wifiDirectR2").invoke(null)),
                    "genuine bp4a generation must enable read-only R2");
        }
        for (String name : new String[] {"WifiP2pNsdValuesContractTest", "WifiP2pNsdValuesExtendedTest"})
            provider("gesher.services." + name, loader, loader);
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(0x24681357); require(parcel.marshall().length == 4, "ordinary native int extent");
            parcel.setDataPosition(0); require(parcel.readInt() == 0x24681357 && parcel.dataAvail() == 0, "ordinary native int reader");
        } finally { parcel.recycle(); }
        System.out.println("P2P_NSD_ORACLE_PROVIDERS graph=" + (stock ? "stock" : "owned")
                + "; genuine boot Parcel; original readers verified before corpus");
    }
    private static void invoke(Class<?> fixture, boolean usd) throws Exception {
        try { fixture.getMethod("run", boolean.class, boolean.class).invoke(null, true, usd); }
        catch (InvocationTargetException failure) { failure.getCause().printStackTrace(System.err); throw failure; }
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2 && (args[0].equals("stock") || args[0].equals("candidate") || args[0].equals("pinned-r4")),
                "stock|candidate|pinned-r4 <payload.jar>");
        require(Build.VERSION.SDK_INT == 36, "API36 oracle required");
        boolean stock = args[0].equals("stock"), pinned = args[0].equals("pinned-r4");
        ClassLoader parent = WifiP2pNsdValuesOracle.class.getClassLoader();
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent) : new OwnedLoader(args[1], parent, pinned);
        System.out.println("P2P_NSD_ORACLE_PROFILE mode=" + args[0] + "; sdk=" + Build.VERSION.SDK_INT
                + "; uid=" + android.os.Process.myUid() + "; vm=" + System.getProperty("java.vm.name")
                + "; fingerprint=" + Build.FINGERPRINT);
        graph(loader, stock, pinned);
        if (stock) {
            for (String name : new String[] {PREFIX + CONFIG[0], "com.android.wifi.flags.Flags"}) {
                try { System.out.println("P2P_NSD_STOCK_OPTIONAL " + name + "=" + type(name, loader).getClassLoader()); }
                catch (ClassNotFoundException absent) { System.out.println("P2P_NSD_STOCK_OPTIONAL " + name + "=absent"); }
            }
            System.out.println("P2P_NSD_STOCK_SCOPE legacy ordinary values; USD not qualified against another release profile");
        }
        invoke(type("gesher.services.WifiP2pNsdValuesContractTest", loader), !stock);
        invoke(type("gesher.services.WifiP2pNsdValuesExtendedTest", loader), !stock);
        System.out.println("P2P_NSD_ORACLE_COMPLETE mode=" + args[0] + "; nativeParcel=true; fullReaders=true; usd=" + !stock);
    }
}
