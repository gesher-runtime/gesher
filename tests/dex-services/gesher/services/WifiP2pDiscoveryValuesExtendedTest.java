/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.OuiKeyedData;
import android.net.wifi.p2p.WifiP2pDiscoveryConfig;
import android.net.wifi.p2p.nsd.WifiP2pUsdBasedServiceResponse;
import android.os.PersistableBundle;
import java.util.ArrayList;
import java.util.List;
import static gesher.services.WifiP2pDiscoveryValuesContractTest.*;

/** Source-level hidden callers, excluded from the SDK36 declaration denominator. */
public final class WifiP2pDiscoveryValuesExtendedTest {
    static OuiKeyedData vendor(int oui, String label, int count) {
        PersistableBundle bundle = new PersistableBundle();
        bundle.putString("label", label); bundle.putInt("count", count);
        bundle.putIntArray("channels", new int[] {1, 6, 11});
        return new OuiKeyedData.Builder(oui, bundle).build();
    }
    static void vendors(boolean parcel) throws Exception {
        OuiKeyedData first = vendor(0x001122, "one", 1), second = vendor(0x001122, "two", 2);
        List<OuiKeyedData> input = new ArrayList<>(); input.add(first);
        WifiP2pDiscoveryConfig.Builder builder = new WifiP2pDiscoveryConfig.Builder(0);
        check(builder.setVendorData(input) == builder, "hidden vendor setter chains");
        WifiP2pDiscoveryConfig one = builder.build();
        check(one.getVendorData() != input && one.getVendorData().get(0) == first, "build shallow-copies vendor list");
        input.add(second);
        equal(1, one.getVendorData().size(), "built vendor list independent of caller list");
        WifiP2pDiscoveryConfig duplicate = builder.build();
        equal(2, duplicate.getVendorData().size(), "builder retains caller list alias");
        check(duplicate.getVendorData().get(0) == first && duplicate.getVendorData().get(1) == second,
                "duplicate OUIs and element identities retained");
        one.getVendorData().add(second); equal(2, one.getVendorData().size(), "vendor getter is mutable");
        one.getVendorData().remove(1); equal(2, input.size(), "getter mutation does not mutate caller list");
        first.getData().putInt("count", 7);
        equal(7, duplicate.getVendorData().get(0).getData().getInt("count"), "list copy retains vendor object alias");
        first.getData().putInt("count", 1);
        rejects(IllegalArgumentException.class, "setVendorData received a null value", () -> builder.setVendorData(null),
                "hidden vendor setter null");
        equal(2, builder.build().getVendorData().size(), "vendor rejection preserves previous list");
        equal(0, new WifiP2pDiscoveryConfig.Builder(0).build().getVendorData().size(), "hidden default vendor list empty");
        if (parcel) {
            WifiP2pDiscoveryConfig oneCopy = wire("discovery_vendor_one", one, WifiP2pDiscoveryConfig.CREATOR);
            WifiP2pDiscoveryConfig duplicateCopy = wire("discovery_vendor_duplicate_oui", duplicate, WifiP2pDiscoveryConfig.CREATOR);
            equal(one.getVendorData(), oneCopy.getVendorData(), "full typed list reader values");
            equal(duplicate.getVendorData(), duplicateCopy.getVendorData(), "full duplicate OUI reader values");
            check(one.getVendorData() != oneCopy.getVendorData() && first != oneCopy.getVendorData().get(0),
                    "full typed list reader creates fresh list and vendor object");
            equal(OuiKeyedData.class, oneCopy.getVendorData().get(0).getClass(), "typed list reader uses coherent owned OuiKeyedData");
            check(first.getData() != oneCopy.getVendorData().get(0).getData(), "native bundle reader creates fresh bundle");
        }
    }
    public static void run(boolean parcel, boolean modern) throws Exception {
        vendors(parcel);
        if (!modern) return;
        byte[] bytes = {0, 1, 127, -128, -1};
        WifiP2pUsdBasedServiceResponse response = new WifiP2pUsdBasedServiceResponse(255, bytes);
        identity(response, new WifiP2pUsdBasedServiceResponse(255, bytes), "response");
        bytes[1] = 17; equal((byte) 17, response.getServiceSpecificInfo()[1], "response caller mutation");
        response.getServiceSpecificInfo()[1] = 1; equal((byte) 1, bytes[1], "response getter mutation");
        response(new WifiP2pUsdBasedServiceResponse(0, null), 0, null, parcel, "response_null");
        byte[] empty = new byte[0];
        response(new WifiP2pUsdBasedServiceResponse(1, empty), 1, empty, parcel, "response_empty");
        response(response, 255, bytes, parcel, "response_bytes");
        if (!parcel) {
            equal(-1, new WifiP2pUsdBasedServiceResponse(-1, null).getServiceProtocolType(), "hidden constructor no lower bound");
            equal(256, new WifiP2pUsdBasedServiceResponse(256, null).getServiceProtocolType(), "hidden constructor no upper bound");
        }
    }
}
