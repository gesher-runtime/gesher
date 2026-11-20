/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import dalvik.system.PathClassLoader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.ParameterizedType;

/** Coherent value providers, genuine boot Parcel, and identity checks before any corpus. */
public final class WifiP2pValuesOracle {
    private static final String[] VALUE_GRAPH = {
        "android.net.wifi.p2p.WifiP2pConfig", "android.net.wifi.p2p.WifiP2pPairingBootstrappingConfig",
        "android.net.wifi.p2p.WifiP2pDevice", "android.net.wifi.p2p.WifiP2pWfdInfo",
        "android.net.wifi.p2p.WifiP2pDeviceList", "android.net.wifi.p2p.WifiP2pGroup", "android.net.wifi.p2p.WifiP2pInfo",
        "android.net.wifi.WpsInfo", "android.net.wifi.WifiInfo", "android.net.wifi.WifiConfiguration",
        "android.net.wifi.WifiEnterpriseConfig", "android.net.wifi.SecurityParams", "android.net.wifi.ParcelUtil",
        "android.net.wifi.OuiKeyedData", "android.net.wifi.util.PersistableBundleUtils", "android.net.wifi.WifiSsid",
        "android.net.wifi.util.HexEncoding", "android.net.wifi.SupplicantState", "android.net.wifi.MloLink",
        "android.net.wifi.ScanResult", "android.net.wifi.ScanResultSecurity", "android.net.wifi.AnqpInformationElement",
        "android.net.MacAddress", "android.net.TransportInfo"
    };
    private static final String[] STOCK_GRAPH = {
        "android.net.wifi.p2p.WifiP2pConfig", "android.net.wifi.p2p.WifiP2pDevice",
        "android.net.wifi.p2p.WifiP2pWfdInfo", "android.net.wifi.p2p.WifiP2pDeviceList",
        "android.net.wifi.p2p.WifiP2pGroup", "android.net.wifi.p2p.WifiP2pInfo",
        "android.net.wifi.WpsInfo", "android.net.MacAddress", "android.net.wifi.ScanResult$InformationElement"
    };
    private static final String[] PINNED_EXTRA = {
        "android.net.wifi.p2p.WifiP2pDirInfo", "android.net.wifi.util.Environment",
        "com.android.wifi.flags.Flags", "com.android.wifi.flags.FeatureFlags", "com.android.wifi.flags.FeatureFlagsImpl"
    };
    private static final class OwnedLoader extends PathClassLoader {
        private final boolean pinned;
        OwnedLoader(String path, ClassLoader parent, boolean pinned) { super(path, parent); this.pinned = pinned; }
        private boolean owns(String name) {
            if (name.startsWith("gesher.services.WifiP2pValues")) return true;
            for (String value : VALUE_GRAPH) if (name.equals(value) || name.startsWith(value + "$")) return true;
            if (pinned) for (String value : PINNED_EXTRA)
                if (name.equals(value) || name.startsWith(value + "$")) return true;
            return false;
        }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!owns(name)) return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name); // no compilation-stub/stock fallback
            if (resolve) resolveClass(type);
            return type;
        }
    }
    private static void require(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    private static Class<?> type(String name, ClassLoader loader) throws Exception { return Class.forName(name, true, loader); }
    private static void graph(ClassLoader loader, boolean stock, boolean pinned) throws Exception {
        ClassLoader boot = Parcel.class.getClassLoader();
        require(boot == null || boot.getClass().getName().endsWith("BootClassLoader"), "Parcel must be an actual boot provider");
        require(type("android.os.Parcel", loader) == Parcel.class && type("android.os.Parcelable", loader) == Parcelable.class,
                "No Android Parcel/Parcelable replacement is permitted");
        for (String name : stock ? STOCK_GRAPH : VALUE_GRAPH) {
            Class<?> value = type(name, loader);
            require(value.getClassLoader() == (stock ? boot : loader), "Provider mismatch: " + name);
            System.out.println("P2P_PROVIDER " + name + "=" + value.getClassLoader());
        }
        if (pinned) for (String name : PINNED_EXTRA) {
            require(type(name, loader).getClassLoader() == loader, "Pinned helper fell back: " + name);
            System.out.println("P2P_PROVIDER " + name + "=" + loader);
        }
        Class<?> config = type("android.net.wifi.p2p.WifiP2pConfig", loader);
        Class<?> builder = type("android.net.wifi.p2p.WifiP2pConfig$Builder", loader);
        Class<?> device = type("android.net.wifi.p2p.WifiP2pDevice", loader);
        Class<?> mac = type("android.net.MacAddress", loader);
        Class<?> wps = type("android.net.wifi.WpsInfo", loader);
        Class<?> wfd = type("android.net.wifi.p2p.WifiP2pWfdInfo", loader);
        Class<?> infoElement = type("android.net.wifi.ScanResult$InformationElement", loader);
        require(config.getField("wps").getType() == wps, "Config WPS identity");
        require(builder.getMethod("setDeviceAddress", mac).getParameterTypes()[0] == mac, "Config MAC identity");
        require(device.getMethod("getWfdInfo").getReturnType() == wfd, "Device WFD identity");
        require(((ParameterizedType) device.getMethod("getVendorElements").getGenericReturnType())
                .getActualTypeArguments()[0] == infoElement, "Device InformationElement identity");
        require(type("android.net.wifi.p2p.WifiP2pGroup", loader).getMethod("getOwner").getReturnType() == device,
                "Group owner identity");
        require(type("android.net.wifi.p2p.WifiP2pDeviceList", loader).getMethod("get", String.class).getReturnType() == device,
                "DeviceList identity");
        if (!stock) {
            Class<?> pairing = type("android.net.wifi.p2p.WifiP2pPairingBootstrappingConfig", loader);
            require(config.getMethod("getPairingBootstrappingConfig").getReturnType() == pairing, "Config pairing identity");
            require(builder.getMethod("setPairingBootstrappingConfig", pairing).getParameterTypes()[0] == pairing, "Builder pairing identity");
            require(type("gesher.services.WifiP2pValuesExtendedTest", loader).getMethod("ordinaryList",
                    java.lang.reflect.Array.newInstance(device, 0).getClass()).getReturnType()
                    == type("android.net.wifi.p2p.WifiP2pDeviceList", loader), "Source setup graph identity");
        }
        boolean bootCoherent = device == Class.forName(device.getName(), false, boot);
        require(bootCoherent == stock, "Unexpected Parcel/null-loader profile");
        System.out.println("P2P_NULL_LOADER bootCoherent=" + bootCoherent
                + "; DeviceList " + (bootCoherent ? "reader and writer qualified by corpus" : "writer only; reader not executed"));
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(0x24681357); require(parcel.marshall().length == 4, "Native Parcel ordinary int extent");
            parcel.setDataPosition(0); require(parcel.readInt() == 0x24681357 && parcel.dataAvail() == 0, "Native Parcel ordinary int");
        } finally { parcel.recycle(); }
        System.out.println("P2P_VALUES_PROVIDERS verified before corpus; genuine native Parcel");
    }
    private static void invoke(Class<?> owner, String name, Class<?>[] types, Object... args) throws Exception {
        try { owner.getMethod(name, types).invoke(null, args); }
        catch (InvocationTargetException failure) {
            failure.getCause().printStackTrace(System.err);
            throw failure;
        }
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2 && (args[0].equals("stock") || args[0].equals("candidate") || args[0].equals("pinned-r4")),
                "stock|candidate|pinned-r4 <payload.jar>");
        require(Build.VERSION.SDK_INT == 36, "API36 required");
        boolean stock = args[0].equals("stock"), pinned = args[0].equals("pinned-r4");
        ClassLoader parent = WifiP2pValuesOracle.class.getClassLoader();
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent) : new OwnedLoader(args[1], parent, pinned);
        System.out.println("P2P_PROFILE mode=" + args[0] + "; sdk=" + Build.VERSION.SDK_INT
                + "; uid=" + android.os.Process.myUid() + "; vm=" + System.getProperty("java.vm.name")
                + "; fingerprint=" + Build.FINGERPRINT);
        graph(loader, stock, pinned);
        if (stock) {
            for (String name : new String[]{"android.net.wifi.p2p.WifiP2pPairingBootstrappingConfig", "com.android.wifi.flags.Flags"}) {
                try { System.out.println("P2P_STOCK_OPTIONAL " + name + "=" + type(name, loader).getClassLoader()); }
                catch (ClassNotFoundException absent) { System.out.println("P2P_STOCK_OPTIONAL " + name + "=absent"); }
            }
            try { System.out.println("P2P_STOCK_R2_FLAG " + type("com.android.wifi.flags.Flags", loader).getMethod("wifiDirectR2").invoke(null)); }
            catch (ReflectiveOperationException unavailable) { System.out.println("P2P_STOCK_R2_FLAG unavailable=" + unavailable.getClass().getSimpleName()); }
        } else if (pinned) {
            require(Boolean.TRUE.equals(type("com.android.wifi.flags.Flags", loader).getMethod("wifiDirectR2").invoke(null)),
                    "Pinned bp4a generator must enable read-only wifi_direct_r2");
        }
        Class<?> fixture = type("gesher.services.WifiP2pValuesContractTest", loader);
        require(fixture.getClassLoader() == loader, "Public fixture provider");
        invoke(fixture, "run", new Class<?>[]{boolean.class, boolean.class, boolean.class}, true, !stock, stock);
        if (!stock) invoke(type("gesher.services.WifiP2pValuesExtendedTest", loader), "run",
                new Class<?>[]{boolean.class, boolean.class}, true, false);
        System.out.println("P2P_ORACLE_COMPLETE mode=" + args[0] + "; nativeParcel=true; DeviceListReader=" + stock);
    }
}
