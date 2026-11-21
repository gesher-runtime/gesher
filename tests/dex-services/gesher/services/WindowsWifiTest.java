/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import com.gesher.net.WindowsNetworks;
import com.gesher.net.WindowsWifi;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Windows Wi-Fi parsing, async scan/timeout handling, and SDK36 model population contracts. */
public final class WindowsWifiTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static void testInterfaceParsingAndOverlay() {
        String sample = ""
                + "There is 1 interface on the system:\r\n"
                + "\r\n"
                + "    Name                   : Wi-Fi\r\n"
                + "    State                  : connected\r\n"
                + "    SSID                   : \u200EGesherTestNet\u200F\r\n"
                + "    AP BSSID               : AA:BB:CC:11:22:33\r\n"
                + "    Radio type             : 802.11ax\r\n"
                + "    Authentication         : WPA2-Personal\r\n"
                + "    Band                   : 5 GHz\r\n"
                + "    Channel                : 36\r\n"
                + "    Receive rate (Mbps)    : 866\r\n"
                + "    Transmit rate (Mbps)   : 720\r\n"
                + "    Signal                 : 80%\r\n";
        WindowsNetworks.Snapshot snap = WindowsNetworks.parse(
                "internet\t1\t1\n"
                        + "net\t6\twifi\t1500\t1000000000\t1000000000\t0\t0\t0\t1\t1\tWi-Fi\tTest\n"
                        + "addr\t6\t10.0.0.148\t24\n"
                        + "gw\t6\t10.0.0.138\n"
                        + "dns\t6\t10.0.0.138\n");
        WindowsNetworks.Net wifiNet = snap.defaultNetwork();
        WindowsWifi.InterfaceState state = WindowsWifi.parseInterfacesOutput(sample, wifiNet);
        require(state.hasInterface && state.enabled && state.connected, "Connected state parsed");
        require("GesherTestNet".equals(state.ssid), "Bidi marks stripped from SSID");
        require("aa:bb:cc:11:22:33".equals(state.bssid), "BSSID normalized lowercase");
        require(state.frequencyMhz == 5180, "5 GHz channel 36 mapped to 5180 MHz");
        require(state.rssi == -60, "Signal 80% mapped to -60 dBm");
        require(state.wifiStandard == ScanResult.WIFI_STANDARD_11AX, "Wi-Fi standard 11ax mapped");
        require(state.securityType == WifiInfo.SECURITY_TYPE_PSK, "WPA2-Personal mapped to PSK");
        require(state.ipAddress == ((148 << 24) | 10), "Little-endian IPv4 address matches");
        require(state.gateway == ((138 << 24) | 10), "Little-endian IPv4 gateway matches");
        require(state.netmask == 0x00ffffff, "Little-endian /24 netmask matches");

        WindowsWifi.InterfaceState empty = WindowsWifi.parseInterfacesOutput(
                "There is no wireless interface on the system.", null);
        require(!empty.hasInterface && !empty.enabled && !empty.connected,
                "Missing interface stays disabled");
    }

    private static void testScanParsingAndSdk36Fields() {
        String sample = ""
                + "SSID 1 : Test_2G\r\n"
                + "    Authentication          : WPA2-Personal\r\n"
                + "    Encryption              : CCMP\r\n"
                + "    BSSID 1                 : 10:20:30:40:50:60\r\n"
                + "         Signal             : 90%\r\n"
                + "         Band               : 2.4 GHz\r\n"
                + "         Channel            : 6\r\n"
                + "SSID 2 : OpenCafe\r\n"
                + "    Authentication          : Open\r\n"
                + "    Encryption              : None\r\n"
                + "    BSSID 1                 : 01:02:03:04:05:06\r\n"
                + "         Signal             : 50%\r\n"
                + "         Band               : 5 GHz\r\n"
                + "         Channel            : 149\r\n";
        List<ScanResult> results = WindowsWifi.parseNetworksOutput(sample);
        require(results.size() == 2, "Two BSSIDs parsed");
        ScanResult first = results.get(0);
        require("Test_2G".equals(first.SSID), "Deprecated ScanResult.SSID populated");
        require(first.getWifiSsid() != null && "Test_2G".equals(first.getWifiSsid().getUtf8Text().toString()),
                "ScanResult.getWifiSsid populated");
        require("10:20:30:40:50:60".equals(first.BSSID), "First BSSID matches");
        require("[WPA2-PSK-CCMP][ESS]".equals(first.capabilities), "WPA2 capabilities string");
        require(first.frequency == 2437 && first.level == -55, "Channel 6 frequency & RSSI");

        ScanResult second = results.get(1);
        require("OpenCafe".equals(second.SSID) && second.frequency == 5745 && second.level == -75,
                "Second scan entry matches");
    }

    private static void testSubprocessTimeoutBoundsExecution() throws Exception {
        Method exec = WindowsWifi.class.getDeclaredMethod("execCommand", String[].class, int.class);
        exec.setAccessible(true);
        String os = System.getProperty("os.name", "");
        if (!os.toLowerCase().contains("win")) {
            checks++;
            return;
        }
        String[] fast = {"/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
                "-NoProfile", "-NonInteractive", "-Command", "Write-Output ok-fast"};
        String fastOut = (String) exec.invoke(null, (Object) fast, 5);
        require(fastOut != null && fastOut.contains("ok-fast"), "Fast subprocess output returned");

        String[] slow = {"/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
                "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Milliseconds 6000; Write-Output late"};
        long startNs = System.nanoTime();
        String timedOut = (String) exec.invoke(null, (Object) slow, 1);
        long elapsedMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - startNs);
        require("".equals(timedOut) && elapsedMs < 3500,
                "Slow subprocess bounded by timeout instead of blocking on stream read (" + elapsedMs + "ms)");
    }

    private static void testLiveWifiAdapterWhenPresent() throws Exception {
        Constructor<WifiManager> ctor = WifiManager.class.getDeclaredConstructor(android.content.Context.class);
        ctor.setAccessible(true);
        WifiManager mgr = ctor.newInstance((android.content.Context) null);
        boolean enabled = mgr.isWifiEnabled();
        require((enabled ? WifiManager.WIFI_STATE_ENABLED : WifiManager.WIFI_STATE_DISABLED) == mgr.getWifiState(),
                "WifiState agrees with isWifiEnabled");
        WifiInfo info = mgr.getConnectionInfo();
        require(info != null && info.getSSID() != null, "ConnectionInfo never null");
        DhcpInfo dhcp = mgr.getDhcpInfo();
        List<WifiConfiguration> configs = mgr.getConfiguredNetworks();
        require(dhcp != null && configs != null, "DhcpInfo and ConfiguredNetworks non-null");
        long t0 = System.nanoTime();
        List<ScanResult> scans = mgr.getScanResults();
        long scanMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - t0);
        require(scans != null && scanMs < 1500, "getScanResults returns without blocking (" + scanMs + "ms)");
        if (!enabled) {
            require(!mgr.startScan(), "startScan returns false when Wi-Fi is disabled/absent");
        } else {
            boolean accepted = mgr.startScan();
            require(accepted, "startScan returns true asynchronously when Wi-Fi is enabled");
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        testInterfaceParsingAndOverlay();
        testScanParsingAndSdk36Fields();
        testSubprocessTimeoutBoundsExecution();
        testLiveWifiAdapterWhenPresent();
        System.out.println("WindowsWifiTest all passed: " + checks);
    }
}
