/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.BadParcelableException;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.gesher.net.IWindowsWifi;
import java.util.ArrayList;
import java.util.List;

/** Production transport on ART's real Parcel/Binder natives, through a forced proxy.
 * The explicit endpoint supplies fixture observations; it is not a real WLAN scan. */
public final class WindowsWifiTransportTest {
    private static int checks;
    private interface Checked { void run() throws Exception; }
    private static void check(boolean condition, String why) {
        if (!condition) throw new AssertionError(why); checks++;
    }
    private static void rejects(Class<? extends Throwable> expected, Checked operation) throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected.getSimpleName() + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static final class Endpoint extends IWindowsWifi.Stub {
        int calls;
        int scanCount = 2;
        int interfaceIndex;
        boolean includedLocation;
        String name, tag;
        boolean scanAccepted = true, empty;
        void record(String name, String tag) {
            if ("denied".equals(name)) throw new SecurityException("fixture permission denied");
            this.name = name; this.tag = tag; calls++;
        }
        @Override public int getWifiState(String name) { record(name, null); return WifiManager.WIFI_STATE_ENABLED; }
        @Override public WifiInfo getConnectionInfo(String name, String tag) {
            record(name, tag); if (empty) return null;
            WifiInfo result = new WifiInfo();
            result.setSSID(WifiSsid.fromBytes(new byte[]{65, 0, 66}));
            result.setBSSID("02:10:20:30:40:50"); result.setFrequency(5180); result.setRssi(-49);
            return result;
        }
        @Override public WifiInfo getConnectionInfoForNetwork(String name, String tag, int index, boolean includeLocation) {
            interfaceIndex = index; includedLocation = includeLocation;
            return getConnectionInfo(name, tag);
        }
        @Override public DhcpInfo getDhcpInfo(String name) {
            record(name, null); if (empty) return null;
            DhcpInfo result = new DhcpInfo(); result.ipAddress = 0x0501a8c0; result.gateway = 0x0101a8c0;
            result.netmask = 0x00ffffff; result.dns1 = 0x08080808; result.dns2 = 0x01010101;
            result.serverAddress = 0x0101a8c0; result.leaseDuration = 2400; return result;
        }
        @Override public List<WifiConfiguration> getConfiguredNetworks(String name, String tag) {
            record(name, tag); if (empty) return new ArrayList<>();
            WifiConfiguration result = new WifiConfiguration(); result.SSID = "\"Fixture profile\"";
            result.networkId = 19; result.hiddenSSID = true; return List.of(result);
        }
        @Override public List<ScanResult> getScanResults(String name, String tag) {
            record(name, tag); if (empty) return new ArrayList<>();
            ScanResult first = new ScanResult(); first.SSID = "Fixture AP"; first.BSSID = "02:10:20:30:40:50";
            first.capabilities = "[WPA2-PSK-CCMP][ESS]"; first.frequency = 5180; first.level = -49;
            first.timestamp = 123456789L; first.wifiSsid = WifiSsid.fromBytes(new byte[]{65, 0, 66});
            ScanResult.InformationElement element = new ScanResult.InformationElement();
            element.id = 48; element.bytes = new byte[]{1, 0, 0, 15, -84, 4};
            first.informationElements = new ScanResult.InformationElement[]{element};
            ScanResult second = new ScanResult(first); second.SSID = ""; second.BSSID = "02:10:20:30:40:51";
            second.frequency = 2412; second.timestamp = 123456790L;
            List<ScanResult> result = new ArrayList<>(); result.add(first); result.add(second);
            for (int i = 2; i < scanCount; i++) {
                ScanResult extra = new ScanResult(first); extra.timestamp += i; result.add(extra);
            }
            return result;
        }
        @Override public boolean startScan(String name, String tag) { record(name, tag); return scanAccepted; }
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    private static void malformed(IBinder binder, String descriptor, boolean trailing, boolean network) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(descriptor); data.writeString("fixture.app"); data.writeString("wifi");
            if (network) { data.writeInt(37); data.writeBoolean(true); }
            if (trailing) data.writeInt(42);
            binder.transact(IBinder.FIRST_CALL_TRANSACTION + (network ? 6 : 5), data, reply, 0);
            reply.readException();
        } finally { data.recycle(); reply.recycle(); }
    }
    public static void main(String[] args) throws Exception {
        // Exercise real Binder/Parcel JNI, as in the other ART integration fixtures.
        System.loadLibrary("android_runtime");
        Endpoint endpoint = new Endpoint();
        check(IWindowsWifi.Stub.asInterface(null) == null, "Null service binder");
        check(IWindowsWifi.Stub.asInterface(endpoint) == endpoint, "Local service path");
        IBinder transport = remote(endpoint);
        IWindowsWifi wifi = IWindowsWifi.Stub.asInterface(transport);
        check(wifi != endpoint && wifi.asBinder() == transport, "Forced proxy path");
        check(wifi.getWifiState("fixture.app") == WifiManager.WIFI_STATE_ENABLED, "Integer state round trip");
        check("fixture.app".equals(endpoint.name) && endpoint.tag == null, "State package attribution");
        WifiInfo info = wifi.getConnectionInfo("fixture.app", "wifi");
        check("wifi".equals(endpoint.tag), "Connection attribution tag");
        check("02:10:20:30:40:50".equals(info.getBSSID()) && info.getFrequency() == 5180
                && info.getRssi() == -49, "WifiInfo typed object");
        check(java.util.Arrays.equals(info.getWifiSsid().getBytes(), new byte[]{65, 0, 66}), "Raw SSID bytes survive transport");
        WifiInfo network = wifi.getConnectionInfoForNetwork("fixture.network", "callback", 37, true);
        check(network.getFrequency() == 5180 && endpoint.interfaceIndex == 37 && endpoint.includedLocation
                && "fixture.network".equals(endpoint.name) && "callback".equals(endpoint.tag),
                "Connectivity transports exact index, caller attribution and location opt-in");
        wifi.getConnectionInfoForNetwork("fixture.network", null, 38, false);
        check(endpoint.interfaceIndex == 38 && !endpoint.includedLocation && endpoint.tag == null,
                "Connectivity transports location opt-out");
        DhcpInfo dhcp = wifi.getDhcpInfo("fixture.app");
        check(dhcp.ipAddress == 0x0501a8c0 && dhcp.gateway == 0x0101a8c0 && dhcp.netmask == 0x00ffffff,
                "DHCP address fields");
        check(dhcp.dns1 == 0x08080808 && dhcp.dns2 == 0x01010101
                && dhcp.serverAddress == 0x0101a8c0 && dhcp.leaseDuration == 2400, "DHCP remaining fields");
        List<WifiConfiguration> configured = wifi.getConfiguredNetworks("fixture.app", "profiles");
        check(configured.size() == 1 && configured.get(0).networkId == 19 && configured.get(0).hiddenSSID
                && "\"Fixture profile\"".equals(configured.get(0).SSID), "WifiConfiguration typed list");
        check("profiles".equals(endpoint.tag), "Configuration attribution tag");
        List<ScanResult> scans = wifi.getScanResults("fixture.app", "scan-cache");
        check(scans.size() == 2 && scans.get(0).timestamp == 123456789L && scans.get(1).frequency == 2412,
                "ScanResult typed list retains order, age and channel");
        check(java.util.Arrays.equals(scans.get(0).wifiSsid.getBytes(), new byte[]{65, 0, 66})
                && scans.get(0).informationElements[0].bytes[4] == -84, "Nested scan payload bytes");
        check("scan-cache".equals(endpoint.tag), "Scan cache attribution tag");
        scans.get(0).informationElements[0].bytes[0] = 99;
        check(wifi.getScanResults("fixture.app", null).get(0).informationElements[0].bytes[0] == 1,
                "Remote result is detached from service state");
        endpoint.scanCount = 1024;
        List<ScanResult> dense = wifi.getScanResults("fixture.app", "dense");
        check(dense.size() == 1024 && dense.get(1023).timestamp == 123457812L,
                "Large scan cache crosses list-slice transactions without truncation");
        endpoint.scanCount = 2;
        check(wifi.startScan("fixture.app", "request"), "Accepted boolean scan result");
        check("request".equals(endpoint.tag), "Scan request attribution tag");
        endpoint.scanAccepted = false;
        check(!wifi.startScan("fixture.app", null), "Rejected boolean scan result");
        endpoint.empty = true;
        check(wifi.getConnectionInfo("fixture.app", null) == null && wifi.getDhcpInfo("fixture.app") == null,
                "Null typed objects");
        check(wifi.getConfiguredNetworks("fixture.app", null).isEmpty()
                && wifi.getScanResults("fixture.app", null).isEmpty(), "Empty typed lists");
        rejects(SecurityException.class, () -> wifi.startScan("denied", null));
        int before = endpoint.calls;
        rejects(SecurityException.class, () -> malformed(transport, "wrong.interface", false, false));
        rejects(BadParcelableException.class, () -> malformed(transport, IWindowsWifi.DESCRIPTOR, true, false));
        rejects(BadParcelableException.class, () -> malformed(transport, IWindowsWifi.DESCRIPTOR, true, true));
        check(endpoint.calls == before, "Malformed transactions cannot reach the service");
        IWindowsWifi unavailable = IWindowsWifi.Stub.asInterface(new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
        });
        rejects(RemoteException.class, () -> unavailable.getWifiState("fixture.app"));
        System.out.println("WindowsWifiTransportTest passed: " + checks + " checks; proxy=true");
    }
}
