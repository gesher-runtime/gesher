/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import dalvik.system.PathClassLoader;
import java.lang.reflect.InvocationTargetException;

/** Closed complete source-owned graphs; only genuine boot framework/MAC leaves. */
public final class WifiP2pDiscoveryValuesOracle {
    private static final String PREFIX = "android.net.wifi.p2p.";
    private static final String[] TYPES = {"WifiP2pDiscoveryConfig", "WifiP2pDiscoveryConfig$Builder",
            "WifiP2pUsdBasedLocalServiceAdvertisementConfig", "WifiP2pUsdBasedLocalServiceAdvertisementConfig$Builder",
            "WifiP2pUsdBasedServiceDiscoveryConfig", "WifiP2pUsdBasedServiceDiscoveryConfig$Builder",
            "WifiP2pDirInfo", "nsd.WifiP2pUsdBasedServiceResponse"};
    private static final String[] HELPERS = {"android.net.wifi.OuiKeyedData", "android.net.wifi.util.PersistableBundleUtils",
            "android.internal.modules.utils.build.SdkLevel"};
    private static final class OwnedLoader extends PathClassLoader {
        OwnedLoader(String path, ClassLoader parent) { super(path, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean owned = name.startsWith("gesher.services.WifiP2pDiscoveryValues");
            for (String type : TYPES) owned |= name.equals(PREFIX + type) || name.startsWith(PREFIX + type + "$")
                    || name.equals(PREFIX + type + "-IA");
            for (String type : HELPERS) owned |= name.equals(type) || name.startsWith(type + "$") || name.equals(type + "-IA");
            if (!owned) return super.loadClass(name, resolve);
            Class<?> value = findLoadedClass(name);
            if (value == null) value = findClass(name); // selected values/helpers never fall back to stock or SDK stubs
            if (resolve) resolveClass(value);
            return value;
        }
    }
    private static void require(boolean value, String label) { if (!value) throw new AssertionError(label); }
    private static Class<?> type(String name, ClassLoader loader) throws Exception { return Class.forName(name, true, loader); }
    private static void provider(String name, ClassLoader lookup, ClassLoader expected) throws Exception {
        require(type(name, lookup).getClassLoader() == expected, "Wrong provider: " + name);
        System.out.println("P2P_DISCOVERY_PROVIDER " + name + "=" + expected);
    }
    private static boolean stockPresence(ClassLoader loader) throws Exception {
        boolean complete = true;
        for (String name : TYPES) {
            try { System.out.println("P2P_DISCOVERY_STOCK_PRESENCE " + PREFIX + name + "=" + type(PREFIX + name, loader).getClassLoader()); }
            catch (ClassNotFoundException absent) {
                complete = false;
                System.out.println("P2P_DISCOVERY_STOCK_PRESENCE " + PREFIX + name + "=absent");
            }
        }
        require(type(PREFIX + TYPES[0], loader) != null && type(PREFIX + TYPES[1], loader) != null,
                "Stock DiscoveryConfig and Builder required for common comparison");
        return complete;
    }
    private static void graph(ClassLoader loader, boolean stock, boolean modern) throws Exception {
        ClassLoader boot = Parcel.class.getClassLoader(), owner = stock ? boot : loader;
        require(boot == null || boot.getClass().getName().endsWith("BootClassLoader"), "Genuine boot Parcel required");
        for (Class<?> leaf : new Class<?>[] {Parcel.class, Parcelable.class, PersistableBundle.class, Build.class, MacAddress.class})
            require(type(leaf.getName(), loader) == leaf && leaf.getClassLoader() == boot, "Genuine boot leaf identity: " + leaf);
        for (int i = 0; i < (modern ? TYPES.length : 2); i++) {
            Class<?> value = type(PREFIX + TYPES[i], loader);
            provider(value.getName(), loader, owner);
            require(value.getSuperclass() == Object.class, "Value/builder original Object ancestry");
            if (!TYPES[i].endsWith("$Builder")) {
                require(Parcelable.class.isAssignableFrom(value), "Genuine boot Parcelable identity");
                require(value.getField("CREATOR").get(null).getClass().getClassLoader() == owner, "Original value CREATOR provider");
            }
        }
        provider(HELPERS[0], loader, owner); provider(HELPERS[0] + "$1", loader, owner); provider(HELPERS[0] + "$Builder", loader, owner);
        Class<?> oui = type(HELPERS[0], loader);
        require(oui.getMethod("getData").getReturnType() == PersistableBundle.class, "Vendor getter boot PersistableBundle identity");
        require(type(HELPERS[0] + "$Builder", loader).getConstructor(int.class, PersistableBundle.class).getParameterTypes()[1]
                == PersistableBundle.class, "Vendor builder boot PersistableBundle identity");
        require(oui.getField("CREATOR").get(null).getClass().getClassLoader() == owner, "Original typed-list element reader provider");
        if (!stock) {
            provider(HELPERS[1], loader, loader); provider(HELPERS[2], loader, loader);
            // D8 private-constructor access markers are empty synthetic classes in this same graph.
            for (String name : new String[] {HELPERS[0], PREFIX + TYPES[0], PREFIX + TYPES[2], PREFIX + TYPES[4]})
                provider(name + "-IA", loader, loader);
            require(Boolean.TRUE.equals(type(HELPERS[2], loader).getMethod("isAtLeastV").invoke(null)), "Complete pinned SdkLevel V guard");
            require(Boolean.TRUE.equals(type(HELPERS[2], loader).getMethod("isAtLeastT").invoke(null)), "Complete pinned SdkLevel typed-list guard");
        }
        if (modern) {
            Class<?> dir = type(PREFIX + "WifiP2pDirInfo", loader);
            require(dir.getConstructor(MacAddress.class, byte[].class, byte[].class).getParameterTypes()[0] == MacAddress.class,
                    "DIR constructor boot MacAddress identity");
            require(dir.getMethod("getMacAddress").getReturnType() == MacAddress.class, "DIR getter boot MacAddress identity");
            require(MacAddress.CREATOR.getClass().getClassLoader() == boot, "DIR original boot MacAddress reader identity");
        }
        for (String name : new String[] {"WifiP2pDiscoveryValuesContractTest", "WifiP2pDiscoveryValuesExtendedTest"})
            provider("gesher.services." + name, loader, loader);
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(0x24681357); require(parcel.marshall().length == 4, "Ordinary native int writer extent");
            parcel.setDataPosition(0); require(parcel.readInt() == 0x24681357 && parcel.dataAvail() == 0, "Ordinary native int full reader");
        } finally { parcel.recycle(); }
        System.out.println("P2P_DISCOVERY_ORACLE_PROVIDERS graph=" + (stock ? "stock" : "owned")
                + "; genuine boot Parcel/PersistableBundle/MacAddress; original readers verified before corpus");
    }
    private static void invoke(Class<?> fixture, String method, boolean modern) throws Exception {
        try { fixture.getMethod(method, boolean.class, boolean.class).invoke(null, true, modern); }
        catch (InvocationTargetException failure) { failure.getCause().printStackTrace(System.err); throw failure; }
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2 && (args[0].equals("stock") || args[0].equals("candidate") || args[0].equals("pinned-r4")),
                "stock|candidate|pinned-r4 <payload.jar>");
        require(Build.VERSION.SDK_INT == 36, "API36 oracle required");
        boolean stock = args[0].equals("stock");
        ClassLoader parent = WifiP2pDiscoveryValuesOracle.class.getClassLoader();
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent) : new OwnedLoader(args[1], parent);
        System.out.println("P2P_DISCOVERY_ORACLE_PROFILE mode=" + args[0] + "; sdk=" + Build.VERSION.SDK_INT
                + "; uid=" + android.os.Process.myUid() + "; vm=" + System.getProperty("java.vm.name")
                + "; fingerprint=" + Build.FINGERPRINT);
        boolean modern = !stock || stockPresence(loader);
        graph(loader, stock, modern);
        System.out.println("P2P_DISCOVERY_ORACLE_SCOPE modern=" + modern + "; expectedFullReaders=" + (modern ? 17 : 6));
        Class<?> publicFixture = type("gesher.services.WifiP2pDiscoveryValuesContractTest", loader);
        invoke(publicFixture, "run", modern);
        invoke(type("gesher.services.WifiP2pDiscoveryValuesExtendedTest", loader), "run", modern);
        invoke(publicFixture, "finish", modern);
        System.out.println("P2P_DISCOVERY_ORACLE_COMPLETE mode=" + args[0] + "; nativeParcel=true; fullReaders=true; modern=" + modern);
    }
}
