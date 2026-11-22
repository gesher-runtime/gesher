/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import com.gesher.net.WindowsWifi;
import com.gesher.net.WindowsWifiFixture;
import java.util.concurrent.TimeUnit;

/** Native WLAN parser and nonblocking live API checks. Hardware presence is not assumed. */
public final class WindowsWifiTest {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        WindowsWifiFixture.run(false);
        long start = System.nanoTime();
        int state = WindowsWifi.wifiState();
        if (state != WifiManager.WIFI_STATE_UNKNOWN && state != WifiManager.WIFI_STATE_DISABLED
                && state != WifiManager.WIFI_STATE_ENABLED) throw new AssertionError("Invalid observed WLAN state");
        WifiInfo info = WindowsWifi.connectionInfo();
        if (info == null || info.getNetworkId() != -1 || WindowsWifi.dhcpInfo() == null
                || !WindowsWifi.configuredNetworks().isEmpty() || WindowsWifi.scanResults() == null)
            throw new AssertionError("WLAN read contracts or unknown profile identity");
        if (System.nanoTime() - start > TimeUnit.SECONDS.toNanos(1))
            throw new AssertionError("WLAN read APIs waited for a native query");
        System.out.println("WindowsWifiTest all passed: live nonblocking Windows API boundary");
    }
}
