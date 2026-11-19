/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.OuiKeyedData;
import android.net.wifi.ScanResult;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pDeviceList;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pPairingBootstrappingConfig;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.PersistableBundle;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Objects;

/**
 * Explicit source-level setup for fields normally populated by the framework.
 * Unlike the SDK-only caller, this fixture uses retained hidden value mutators.
 * It supplies no Android implementation or radio results. Native inputs are normal records.
 */
public final class WifiP2pValuesExtendedTest {
    private static int checks;
    private static void check(boolean ok, String label) { checks++; if (!ok) throw new AssertionError(label); }
    private static void equal(Object a, Object b, String label) { check(Objects.equals(a, b), label + ": " + a + " != " + b); }
    public static WifiP2pDeviceList ordinaryList(WifiP2pDevice[] values) {
        return new WifiP2pDeviceList(new ArrayList<>(Arrays.asList(values)));
    }
    private static WifiP2pDevice device(int suffix) throws Exception {
        WifiP2pDevice result = new WifiP2pDevice();
        result.deviceAddress = "02:11:22:33:44:0" + suffix;
        result.deviceName = "owned-\u05d0\u05d1-" + suffix;
        result.primaryDeviceType = "1-0050F204-1";
        result.secondaryDeviceType = "2-0050F204-2";
        result.status = WifiP2pDevice.AVAILABLE;
        result.deviceCapability = 0x01;
        result.groupCapability = 0x01;
        result.wpsConfigMethodsSupported = 0x0080 | 0x0100 | 0x0008;
        result.setInterfaceMacAddress(MacAddress.fromString("02:aa:bb:cc:dd:0" + suffix));
        result.setIpAddress(InetAddress.getByAddress(new byte[]{(byte) 192, 0, 2, (byte) suffix}));
        result.wfdInfo = new WifiP2pWfdInfo();
        result.wfdInfo.setEnabled(true);
        result.wfdInfo.setDeviceType(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK);
        result.wfdInfo.setR2DeviceType(WifiP2pWfdInfo.DEVICE_TYPE_WFD_SOURCE);
        result.wfdInfo.setSessionAvailable(true);
        result.wfdInfo.setControlPort(7236);
        result.wfdInfo.setMaxThroughput(150);
        ScanResult.InformationElement element = new ScanResult.InformationElement();
        element.id = 221; element.idExt = 0; element.bytes = new byte[]{0x12, 0x34, 0x56, 0, (byte) 255, (byte) suffix};
        result.setVendorElements(new ArrayList<>(Arrays.asList(element)));
        PersistableBundle data = new PersistableBundle();
        data.putString("name", "\u05e9\u05dc\u05d5\u05dd\u0000" + suffix);
        data.putIntArray("values", new int[]{0, -1, suffix});
        result.setVendorData(new ArrayList<>(Arrays.asList(new OuiKeyedData.Builder(0x123456, data).build())));
        result.setPairingBootStrappingMethods(
                WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC
                | WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PINCODE
                | WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PINCODE
                | WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE
                | WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PASSPHRASE);
        return result;
    }
    private static void aliases() throws Exception {
        WifiP2pDevice original = device(1);
        check(original.isGroupOwner() && original.isServiceDiscoveryCapable(), "supplied capability bits");
        check(original.wpsPbcSupported() && original.wpsDisplaySupported() && original.wpsKeypadSupported(), "supplied WPS bits");
        check(original.isOpportunisticBootstrappingMethodSupported() && original.isPinCodeDisplayBootstrappingMethodSupported()
                && original.isPinCodeKeypadBootstrappingMethodSupported()
                && original.isPassphraseDisplayBootstrappingMethodSupported()
                && original.isPassphraseKeypadBootstrappingMethodSupported(), "supplied pairing bits");
        WifiP2pDevice copy = new WifiP2pDevice(original);
        check(copy.wfdInfo != original.wfdInfo, "Device copy duplicates WFD");
        check(copy.getIpAddress() == original.getIpAddress(), "Device copy shares immutable IP");
        check(copy.getVendorElements() != original.getVendorElements(), "vendor list containers separate");
        check(copy.getVendorElements().get(0) == original.getVendorElements().get(0), "vendor elements shared");
        List<ScanResult.InformationElement> view = original.getVendorElements();
        view.clear(); equal(1, original.getVendorElements().size(), "vendor getter detaches list");
        original.getVendorElements().get(0).bytes[0] = 7;
        equal((byte) 7, copy.getVendorElements().get(0).bytes[0], "vendor element mutable alias");
        original.wfdInfo.setControlPort(1000);
        equal(7236, copy.wfdInfo.getControlPort(), "WFD copy independent");
        WifiP2pDevice update = device(1); update.deviceName = "updated"; update.status = WifiP2pDevice.CONNECTED;
        original.update(update);
        check(original.wfdInfo == update.wfdInfo, "Device update shares WFD");
        equal(update.status, original.status, "Device update status");

        ArrayList<WifiP2pDevice> inputs = new ArrayList<>(Arrays.asList(original));
        WifiP2pDeviceList list = new WifiP2pDeviceList(inputs);
        check(list.get(original.deviceAddress) != original, "DeviceList constructor copies devices");
        check(list.get(original.deviceAddress).wfdInfo != original.wfdInfo, "DeviceList copies device WFD");
        Collection<WifiP2pDevice> live = list.getDeviceList();
        WifiP2pDevice second = device(2); list.update(second);
        equal(2, live.size(), "DeviceList getter is a live view");
        check(list.get(second.deviceAddress) == second, "new update inserts original object");
        second.deviceName = "mutated";
        equal("mutated", list.get(second.deviceAddress).deviceName, "stored device mutable");
        WifiP2pDeviceList saved = new WifiP2pDeviceList(list);
        check(saved.get(second.deviceAddress) != second, "DeviceList copy duplicates stored devices");
        second.deviceName = "again";
        equal("mutated", saved.get(second.deviceAddress).deviceName, "copied DeviceList stable field");
        WifiP2pDevice replacement = device(2); replacement.setPairingBootStrappingMethods(1);
        list.updateSupplicantDetails(replacement);
        check(list.get(second.deviceAddress) == second, "update preserves stored identity");
        equal(1, second.getPairingBootStrappingMethods(), "hidden DIR omission preserves neighboring pairing update");
        check(second.wfdInfo == replacement.wfdInfo, "list update shares supplied WFD");

        WifiP2pGroup group = group(device(3), device(4));
        Collection<WifiP2pDevice> clients = group.getClientList();
        WifiP2pGroup groupCopy = new WifiP2pGroup(group);
        check(groupCopy.getOwner() != group.getOwner(), "Group copies owner");
        check(groupCopy.getClientList().iterator().next() == clients.iterator().next(), "Group shares client elements");
        group.addClient(device(5)); equal(2, clients.size(), "Group client view remains live");
        equal(1, groupCopy.getClientList().size(), "Group copies client container");
        equal(MacAddress.fromString("02:01:02:03:04:05"), group.getGroupOwnerBssid(), "Group BSSID source bytes");
        check(groupCopy.getGroupOwnerBssid() == null, "r4 Group copy does not carry interfaceAddress");
        group.setIsGroupOwner(false); check(group.getGroupOwnerBssid() == null, "client has no group-owner BSSID");
    }
    private static WifiP2pGroup group(WifiP2pDevice owner, WifiP2pDevice client) {
        WifiP2pGroup group = new WifiP2pGroup();
        group.setNetworkName("DIRECT-a9-\u05d0\u05d1"); group.setOwner(owner); group.setIsGroupOwner(true);
        group.addClient(client); group.setPassphrase("12345-\u05d0\u05d1\u05d2");
        group.setInterface("p2p-owned0"); group.setNetworkId(WifiP2pGroup.NETWORK_ID_PERSISTENT);
        group.setSecurityType(WifiP2pGroup.SECURITY_TYPE_WPA3_SAE); group.setFrequency(5975);
        group.interfaceAddress = new byte[]{2, 1, 2, 3, 4, 5};
        group.setVendorData(owner.getVendorData());
        return group;
    }
    private static void normalParcels(boolean bootCoherent) throws Exception {
        WifiP2pDevice first = device(1), second = device(2);
        WifiP2pDevice deviceCopy = WifiP2pValuesContractTest.wire("device_full_ipv4", first, WifiP2pDevice.CREATOR, true);
        equal(first.getIpAddress(), deviceCopy.getIpAddress(), "Device IP preserved");
        equal(first.getInterfaceMacAddress(), deviceCopy.getInterfaceMacAddress(), "Device MAC preserved");
        equal(first.getPairingBootStrappingMethods(), deviceCopy.getPairingBootStrappingMethods(), "Device pairing bits preserved");
        check(Arrays.equals(first.getVendorElements().get(0).bytes, deviceCopy.getVendorElements().get(0).bytes), "vendor bytes preserved");
        equal(first.getVendorData(), deviceCopy.getVendorData(), "Device vendor bundle preserved");
        second.setIpAddress(InetAddress.getByAddress(new byte[]{0x20, 1, 0x0d, (byte) 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}));
        WifiP2pValuesContractTest.wire("device_full_ipv6", second, WifiP2pDevice.CREATOR, true);
        WifiP2pDeviceList list = new WifiP2pDeviceList(new ArrayList<>(Arrays.asList(first, second)));
        WifiP2pDeviceList listCopy = WifiP2pValuesContractTest.wire("devicelist_two", list, WifiP2pDeviceList.CREATOR, bootCoherent);
        if (bootCoherent) {
            equal(2, listCopy.getDeviceList().size(), "DeviceList native count");
            equal(first.deviceName, listCopy.get(first.deviceAddress).deviceName, "DeviceList native Device identity/content");
            equal(second.getIpAddress(), listCopy.get(second.deviceAddress).getIpAddress(), "DeviceList native IPv6");
        } else {
            System.out.println("P2P_VALUES_READER_PENDING devicelist_two: writer-only, source P2P provider differs from native Parcel loader");
        }
        WifiP2pGroup group = group(first, second);
        WifiP2pGroup copy = WifiP2pValuesContractTest.wire("group_full", group, WifiP2pGroup.CREATOR, true);
        equal(group.getSecurityType(), copy.getSecurityType(), "Group security wire");
        equal(group.getFrequency(), copy.getFrequency(), "Group frequency wire");
        equal(group.getVendorData(), copy.getVendorData(), "Group vendor wire");
        equal(group.getOwner().deviceName, copy.getOwner().deviceName, "Group owner wire");
        equal(1, copy.getClientList().size(), "Group clients wire");
        check(copy.getGroupOwnerBssid() == null, "Group interfaceAddress is not parceled in r4");
    }
    public static void run(boolean nativeParcel, boolean bootCoherentP2p) throws Exception {
        checks = 0;
        aliases();
        if (nativeParcel) normalParcels(bootCoherentP2p);
        System.out.println("WifiP2pValuesExtendedTest passed: " + checks + " checks; parcel=" + nativeParcel
                + "; bootCoherentP2p=" + bootCoherentP2p);
    }
}
