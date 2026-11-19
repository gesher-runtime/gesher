/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.nio.file.Path;

/** Host-only bootstrap; no Wi-Fi implementation or Parcel transport is adapted. */
public final class WifiConfigurationHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected = Path.of(args[1]).toRealPath();
        for (String suffix : new String[]{"WifiConfiguration", "WifiEnterpriseConfig",
                "SecurityParams", "ParcelUtil", "OuiKeyedData", "util.PersistableBundleUtils"}) {
            Class<?> type = Class.forName("android.net.wifi." + suffix);
            Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI())
                    .toRealPath();
            if (!expected.equals(actual)) {
                throw new AssertionError(type.getName() + " came from " + actual
                        + ", expected fresh production output " + expected);
            }
        }
        System.out.println("Wi-Fi value providers: freshly compiled production classes");
        WifiConfigurationContractTest.run(false);
    }
}
