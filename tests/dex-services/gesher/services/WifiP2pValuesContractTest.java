/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.WpsInfo;
import android.net.wifi.p2p.WifiP2pConfig;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pDeviceList;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pPairingBootstrappingConfig;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.Parcel;
import android.os.Parcelable;
import java.net.InetAddress;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Objects;

/** Public call sites compile against the official SDK36 only. No radio operations. */
public final class WifiP2pValuesContractTest {
    private static int checks;
    private static final String PEER = "02:11:22:33:44:55";
    private interface Action { void run() throws Exception; }
    private static void check(boolean result, String label) {
        checks++;
        if (!result) throw new AssertionError(label);
    }
    private static void equal(Object expected, Object actual, String label) {
        check(Objects.equals(expected, actual), label + ": " + expected + " != " + actual);
    }
    private static void rejects(Class<? extends Throwable> expected, Action action, String label) {
        try { action.run(); }
        catch (Throwable actual) {
            check(expected.isInstance(actual), label + ": expected " + expected + ", got " + actual);
            return;
        }
        throw new AssertionError(label + ": missing " + expected.getName());
    }
    private static WifiP2pConfig.Builder peer() {
        return new WifiP2pConfig.Builder().setDeviceAddress(MacAddress.fromString(PEER));
    }
    private static WifiP2pConfig.Builder credentials(String password) {
        return new WifiP2pConfig.Builder().setNetworkName("DIRECT-a9-\u05e9\u05dc\u05d5\u05dd")
                .setPassphrase(password);
    }
    private static WifiP2pDevice device() {
        WifiP2pDevice value = new WifiP2pDevice();
        value.deviceAddress = PEER;
        value.deviceName = "Owned \u05e9\u05dc\u05d5\u05dd \ud83d\udce1 .\\\u0000";
        value.primaryDeviceType = "1-0050F204-1";
        value.secondaryDeviceType = "2-0050F204-2";
        value.status = WifiP2pDevice.AVAILABLE;
        return value;
    }
    private static WifiP2pWfdInfo wfd() {
        WifiP2pWfdInfo value = new WifiP2pWfdInfo();
        value.setEnabled(true);
        check(value.setDeviceType(WifiP2pWfdInfo.DEVICE_TYPE_SOURCE_OR_PRIMARY_SINK), "WFD type");
        check(value.setR2DeviceType(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK), "WFD R2 type");
        value.setSessionAvailable(true);
        value.setContentProtectionSupported(true);
        value.setCoupledSinkSupportAtSource(true);
        value.setCoupledSinkSupportAtSink(true);
        value.setControlPort(7236);
        value.setMaxThroughput(150);
        return value;
    }
    private static void constants() throws Exception {
        equal(WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV4_DHCP, WifiP2pConfig.class.getField("GROUP_CLIENT_IP_PROVISIONING_MODE_IPV4_DHCP").getInt(null), "constant WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV4_DHCP");
        equal(WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV6_LINK_LOCAL, WifiP2pConfig.class.getField("GROUP_CLIENT_IP_PROVISIONING_MODE_IPV6_LINK_LOCAL").getInt(null), "constant WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV6_LINK_LOCAL");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_2GHZ, WifiP2pConfig.class.getField("GROUP_OWNER_BAND_2GHZ").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_BAND_2GHZ");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_5GHZ, WifiP2pConfig.class.getField("GROUP_OWNER_BAND_5GHZ").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_BAND_5GHZ");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_6GHZ, WifiP2pConfig.class.getField("GROUP_OWNER_BAND_6GHZ").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_BAND_6GHZ");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_AUTO, WifiP2pConfig.class.getField("GROUP_OWNER_BAND_AUTO").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_BAND_AUTO");
        equal(WifiP2pConfig.GROUP_OWNER_INTENT_AUTO, WifiP2pConfig.class.getField("GROUP_OWNER_INTENT_AUTO").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_INTENT_AUTO");
        equal(WifiP2pConfig.GROUP_OWNER_INTENT_MAX, WifiP2pConfig.class.getField("GROUP_OWNER_INTENT_MAX").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_INTENT_MAX");
        equal(WifiP2pConfig.GROUP_OWNER_INTENT_MIN, WifiP2pConfig.class.getField("GROUP_OWNER_INTENT_MIN").getInt(null), "constant WifiP2pConfig.GROUP_OWNER_INTENT_MIN");
        equal(WifiP2pConfig.P2P_VERSION_1, WifiP2pConfig.class.getField("P2P_VERSION_1").getInt(null), "constant WifiP2pConfig.P2P_VERSION_1");
        equal(WifiP2pConfig.P2P_VERSION_2, WifiP2pConfig.class.getField("P2P_VERSION_2").getInt(null), "constant WifiP2pConfig.P2P_VERSION_2");
        equal(WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_ONLY, WifiP2pConfig.class.getField("PCC_MODE_CONNECTION_TYPE_LEGACY_ONLY").getInt(null), "constant WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_ONLY");
        equal(WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_OR_R2, WifiP2pConfig.class.getField("PCC_MODE_CONNECTION_TYPE_LEGACY_OR_R2").getInt(null), "constant WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_OR_R2");
        equal(WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_R2_ONLY, WifiP2pConfig.class.getField("PCC_MODE_CONNECTION_TYPE_R2_ONLY").getInt(null), "constant WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_R2_ONLY");
        equal(WifiP2pDevice.AVAILABLE, WifiP2pDevice.class.getField("AVAILABLE").getInt(null), "constant WifiP2pDevice.AVAILABLE");
        equal(WifiP2pDevice.CONNECTED, WifiP2pDevice.class.getField("CONNECTED").getInt(null), "constant WifiP2pDevice.CONNECTED");
        equal(WifiP2pDevice.FAILED, WifiP2pDevice.class.getField("FAILED").getInt(null), "constant WifiP2pDevice.FAILED");
        equal(WifiP2pDevice.INVITED, WifiP2pDevice.class.getField("INVITED").getInt(null), "constant WifiP2pDevice.INVITED");
        equal(WifiP2pDevice.UNAVAILABLE, WifiP2pDevice.class.getField("UNAVAILABLE").getInt(null), "constant WifiP2pDevice.UNAVAILABLE");
        equal(WifiP2pGroup.NETWORK_ID_PERSISTENT, WifiP2pGroup.class.getField("NETWORK_ID_PERSISTENT").getInt(null), "constant WifiP2pGroup.NETWORK_ID_PERSISTENT");
        equal(WifiP2pGroup.NETWORK_ID_TEMPORARY, WifiP2pGroup.class.getField("NETWORK_ID_TEMPORARY").getInt(null), "constant WifiP2pGroup.NETWORK_ID_TEMPORARY");
        equal(WifiP2pGroup.SECURITY_TYPE_UNKNOWN, WifiP2pGroup.class.getField("SECURITY_TYPE_UNKNOWN").getInt(null), "constant WifiP2pGroup.SECURITY_TYPE_UNKNOWN");
        equal(WifiP2pGroup.SECURITY_TYPE_WPA2_PSK, WifiP2pGroup.class.getField("SECURITY_TYPE_WPA2_PSK").getInt(null), "constant WifiP2pGroup.SECURITY_TYPE_WPA2_PSK");
        equal(WifiP2pGroup.SECURITY_TYPE_WPA3_COMPATIBILITY, WifiP2pGroup.class.getField("SECURITY_TYPE_WPA3_COMPATIBILITY").getInt(null), "constant WifiP2pGroup.SECURITY_TYPE_WPA3_COMPATIBILITY");
        equal(WifiP2pGroup.SECURITY_TYPE_WPA3_SAE, WifiP2pGroup.class.getField("SECURITY_TYPE_WPA3_SAE").getInt(null), "constant WifiP2pGroup.SECURITY_TYPE_WPA3_SAE");
        equal(WifiP2pWfdInfo.DEVICE_INFO_AUDIO_ONLY_SUPPORT_AT_SOURCE, WifiP2pWfdInfo.class.getField("DEVICE_INFO_AUDIO_ONLY_SUPPORT_AT_SOURCE").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_AUDIO_ONLY_SUPPORT_AT_SOURCE");
        equal(WifiP2pWfdInfo.DEVICE_INFO_AUDIO_UNSUPPORTED_AT_PRIMARY_SINK, WifiP2pWfdInfo.class.getField("DEVICE_INFO_AUDIO_UNSUPPORTED_AT_PRIMARY_SINK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_AUDIO_UNSUPPORTED_AT_PRIMARY_SINK");
        equal(WifiP2pWfdInfo.DEVICE_INFO_CONTENT_PROTECTION_SUPPORT, WifiP2pWfdInfo.class.getField("DEVICE_INFO_CONTENT_PROTECTION_SUPPORT").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_CONTENT_PROTECTION_SUPPORT");
        equal(WifiP2pWfdInfo.DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SINK, WifiP2pWfdInfo.class.getField("DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SINK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SINK");
        equal(WifiP2pWfdInfo.DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SOURCE, WifiP2pWfdInfo.class.getField("DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SOURCE").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_COUPLED_SINK_SUPPORT_AT_SOURCE");
        equal(WifiP2pWfdInfo.DEVICE_INFO_DEVICE_TYPE_MASK, WifiP2pWfdInfo.class.getField("DEVICE_INFO_DEVICE_TYPE_MASK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_DEVICE_TYPE_MASK");
        equal(WifiP2pWfdInfo.DEVICE_INFO_PREFERRED_CONNECTIVITY_MASK, WifiP2pWfdInfo.class.getField("DEVICE_INFO_PREFERRED_CONNECTIVITY_MASK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_PREFERRED_CONNECTIVITY_MASK");
        equal(WifiP2pWfdInfo.DEVICE_INFO_SESSION_AVAILABLE_MASK, WifiP2pWfdInfo.class.getField("DEVICE_INFO_SESSION_AVAILABLE_MASK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_SESSION_AVAILABLE_MASK");
        equal(WifiP2pWfdInfo.DEVICE_INFO_TDLS_PERSISTENT_GROUP, WifiP2pWfdInfo.class.getField("DEVICE_INFO_TDLS_PERSISTENT_GROUP").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_TDLS_PERSISTENT_GROUP");
        equal(WifiP2pWfdInfo.DEVICE_INFO_TDLS_PERSISTENT_GROUP_REINVOKE, WifiP2pWfdInfo.class.getField("DEVICE_INFO_TDLS_PERSISTENT_GROUP_REINVOKE").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_TDLS_PERSISTENT_GROUP_REINVOKE");
        equal(WifiP2pWfdInfo.DEVICE_INFO_TIME_SYNCHRONIZATION_SUPPORT, WifiP2pWfdInfo.class.getField("DEVICE_INFO_TIME_SYNCHRONIZATION_SUPPORT").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_TIME_SYNCHRONIZATION_SUPPORT");
        equal(WifiP2pWfdInfo.DEVICE_INFO_WFD_SERVICE_DISCOVERY_SUPPORT, WifiP2pWfdInfo.class.getField("DEVICE_INFO_WFD_SERVICE_DISCOVERY_SUPPORT").getInt(null), "constant WifiP2pWfdInfo.DEVICE_INFO_WFD_SERVICE_DISCOVERY_SUPPORT");
        equal(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK, WifiP2pWfdInfo.class.getField("DEVICE_TYPE_PRIMARY_SINK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK");
        equal(WifiP2pWfdInfo.DEVICE_TYPE_SECONDARY_SINK, WifiP2pWfdInfo.class.getField("DEVICE_TYPE_SECONDARY_SINK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_TYPE_SECONDARY_SINK");
        equal(WifiP2pWfdInfo.DEVICE_TYPE_SOURCE_OR_PRIMARY_SINK, WifiP2pWfdInfo.class.getField("DEVICE_TYPE_SOURCE_OR_PRIMARY_SINK").getInt(null), "constant WifiP2pWfdInfo.DEVICE_TYPE_SOURCE_OR_PRIMARY_SINK");
        equal(WifiP2pWfdInfo.DEVICE_TYPE_WFD_SOURCE, WifiP2pWfdInfo.class.getField("DEVICE_TYPE_WFD_SOURCE").getInt(null), "constant WifiP2pWfdInfo.DEVICE_TYPE_WFD_SOURCE");
        equal(WifiP2pWfdInfo.PREFERRED_CONNECTIVITY_P2P, WifiP2pWfdInfo.class.getField("PREFERRED_CONNECTIVITY_P2P").getInt(null), "constant WifiP2pWfdInfo.PREFERRED_CONNECTIVITY_P2P");
        equal(WifiP2pWfdInfo.PREFERRED_CONNECTIVITY_TDLS, WifiP2pWfdInfo.class.getField("PREFERRED_CONNECTIVITY_TDLS").getInt(null), "constant WifiP2pWfdInfo.PREFERRED_CONNECTIVITY_TDLS");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PINCODE, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PINCODE").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PINCODE");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PASSPHRASE, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PASSPHRASE").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PASSPHRASE");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PINCODE, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PINCODE").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PINCODE");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC");
        equal(WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OUT_OF_BAND, WifiP2pPairingBootstrappingConfig.class.getField("PAIRING_BOOTSTRAPPING_METHOD_OUT_OF_BAND").getInt(null), "constant WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OUT_OF_BAND");
    }
    private static void configValues(boolean r4) {
        WifiP2pConfig empty = new WifiP2pConfig();
        equal("", empty.deviceAddress, "default address");
        equal("", empty.getNetworkName(), "default network name");
        equal("", empty.getPassphrase(), "default password");
        equal(WpsInfo.PBC, empty.wps.setup, "default WPS");
        equal(WifiP2pConfig.GROUP_OWNER_INTENT_AUTO, empty.groupOwnerIntent, "default intent");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_AUTO, empty.getGroupOwnerBand(), "default band");
        equal(WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV4_DHCP,
                empty.getGroupClientIpProvisioningMode(), "default IP mode");
        equal(0, empty.describeContents(), "Config contents");
        check(empty.toString().contains("wps:"), "Config text");
        WifiP2pConfig nullCopy = new WifiP2pConfig((WifiP2pConfig) null);
        check(nullCopy.wps == null, "null Config copy does not run default constructor");
        WifiP2pConfig value = peer().enablePersistentMode(true).setGroupOperatingFrequency(5180).build();
        equal(PEER, value.deviceAddress, "peer address");
        equal(WifiP2pGroup.NETWORK_ID_PERSISTENT, value.getNetworkId(), "persistent ID");
        equal(5180, value.getGroupOwnerBand(), "frequency occupies band value");
        value.groupOwnerIntent = 12;
        value.wps.BSSID = PEER;
        value.wps.pin = "01234567";
        WifiP2pConfig copy = new WifiP2pConfig(value);
        check(copy != value && copy.wps != value.wps, "Config/WPS copy identity");
        equal(value.wps.pin, copy.wps.pin, "WPS copy pin");
        equal(value.wps.BSSID, copy.wps.BSSID, "WPS copy BSSID");
        equal(12, copy.groupOwnerIntent, "intent copy");
        value.wps.pin = "changed";
        equal("01234567", copy.wps.pin, "WPS copied independently");
        WifiP2pConfig.Builder builder = peer().enablePersistentMode(false);
        WifiP2pConfig one = builder.build();
        builder.setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_5GHZ);
        WifiP2pConfig two = builder.build();
        equal(0, one.getGroupOwnerBand(), "built value detached from builder");
        equal(WifiP2pConfig.GROUP_OWNER_BAND_5GHZ, two.getGroupOwnerBand(), "builder next value");
        equal(WifiP2pGroup.NETWORK_ID_TEMPORARY, one.getNetworkId(), "temporary ID");
        equal("02:00:00:00:00:00", credentials("12345678").setDeviceAddress(null)
                .build().deviceAddress, "null address resets sentinel with credentials");
        if (r4) {
            equal(WifiP2pConfig.GROUP_OWNER_BAND_6GHZ,
                    peer().setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_6GHZ)
                            .build().getGroupOwnerBand(), "bp4a six GHz value");
            for (int mode : new int[]{WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_ONLY,
                    WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_LEGACY_OR_R2,
                    WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_R2_ONLY}) {
                equal(mode, peer().setPccModeConnectionType(mode).build().getPccModeConnectionType(), "PCC mode");
            }
            WifiP2pPairingBootstrappingConfig pairing = new WifiP2pPairingBootstrappingConfig(
                    WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE,
                    "\u05d0\u05d1\u05d2\ud83d\udd11");
            WifiP2pConfig paired = peer().setPairingBootstrappingConfig(pairing)
                    .setAuthorizeConnectionFromPeerEnabled(true).build();
            check(paired.getPairingBootstrappingConfig() == pairing, "builder retains pairing value");
            check(new WifiP2pConfig(paired).getPairingBootstrappingConfig() == pairing,
                    "Config copy shares pairing value");
            equal(WpsInfo.INVALID, paired.wps.setup, "pairing disables WPS request");
            equal(WifiP2pConfig.GROUP_CLIENT_IP_PROVISIONING_MODE_IPV6_LINK_LOCAL,
                    paired.getGroupClientIpProvisioningMode(), "pairing chooses IPv6 request");
            check(paired.isAuthorizeConnectionFromPeerEnabled(), "authorize request preserved");
            paired.setGroupOwnerVersion(WifiP2pConfig.P2P_VERSION_2);
            equal(WifiP2pConfig.P2P_VERSION_2, paired.getGroupOwnerVersion(), "owner version");
        }
    }
    private static void deviceValues(boolean r4) {
        WifiP2pDevice empty = new WifiP2pDevice();
        equal("", empty.deviceAddress, "default Device address");
        equal(WifiP2pDevice.UNAVAILABLE, empty.status, "default Device status");
        check(!empty.isGroupOwner() && !empty.isServiceDiscoveryCapable(), "default device capability bits");
        check(!empty.wpsPbcSupported() && !empty.wpsDisplaySupported() && !empty.wpsKeypadSupported(),
                "default WPS bits");
        check(empty.getIpAddress() == null && empty.getWfdInfo() == null, "default device optional fields");
        check(empty.getVendorElements().isEmpty(), "default vendor elements");
        if (r4) check(!empty.isOpportunisticBootstrappingMethodSupported()
                && !empty.isPinCodeDisplayBootstrappingMethodSupported()
                && !empty.isPinCodeKeypadBootstrappingMethodSupported()
                && !empty.isPassphraseDisplayBootstrappingMethodSupported()
                && !empty.isPassphraseKeypadBootstrappingMethodSupported(), "default pairing bits");
        WifiP2pDevice value = device();
        WifiP2pDevice copy = new WifiP2pDevice(value);
        equal(value, copy, "Device equality");
        equal(value.hashCode(), copy.hashCode(), "Device hash");
        copy.deviceName = "changed";
        equal(value, copy, "Device equality uses address");
        check(!value.equals(new Object()) && !value.equals(null), "Device equality types");
        copy.deviceAddress = "02:aa:bb:cc:dd:ee";
        check(!value.equals(copy), "Device different address");
        WifiP2pDevice update = device(); update.deviceName = "updated"; update.status = WifiP2pDevice.CONNECTED;
        value.update(update);
        equal("updated", value.deviceName, "Device update name");
        equal(WifiP2pDevice.CONNECTED, value.status, "Device update status");
        check(value.toString().contains(PEER), "Device text");
        equal("", new WifiP2pDevice((WifiP2pDevice) null).deviceAddress, "null Device copy");
        WifiP2pDeviceList list = new WifiP2pDeviceList();
        check(list.getDeviceList().isEmpty(), "empty DeviceList");
        check(list.get(PEER) == null, "absent DeviceList peer");
        check(new WifiP2pDeviceList(list).getDeviceList().isEmpty(), "DeviceList copy");
        check(new WifiP2pDeviceList((WifiP2pDeviceList) null).getDeviceList().isEmpty(), "null DeviceList copy");
        equal(0, list.describeContents(), "DeviceList contents");
    }
    private static void groupAndInfo(boolean r4) throws Exception {
        WifiP2pGroup group = new WifiP2pGroup();
        check(group.getOwner() == null && group.getNetworkName() == null && group.getPassphrase() == null,
                "default Group optional values");
        check(!group.isGroupOwner() && group.getClientList().isEmpty(), "default Group flags/clients");
        equal(0, group.getFrequency(), "default frequency");
        check(group.getInterface() == null, "default interface");
        equal(0, group.getNetworkId(), "default network id");
        WifiP2pGroup copy = new WifiP2pGroup(group);
        check(copy.getOwner() != null, "Group copy creates default owner from null");
        check(new WifiP2pGroup((WifiP2pGroup) null).getOwner() == null, "null Group copy");
        if (r4) {
            equal(WifiP2pGroup.SECURITY_TYPE_WPA2_PSK, group.getSecurityType(), "default Group security");
            check(group.getGroupOwnerBssid() == null, "default Group BSSID");
        }
        check(group.toString().contains("network:"), "Group text");
        WifiP2pInfo info = new WifiP2pInfo();
        check(!info.groupFormed && !info.isGroupOwner && info.groupOwnerAddress == null, "default Info");
        info.groupFormed = true; info.isGroupOwner = true;
        info.groupOwnerAddress = InetAddress.getByAddress(new byte[]{(byte) 192, 0, 2, 3});
        WifiP2pInfo saved = new WifiP2pInfo(info);
        check(saved.groupFormed && saved.isGroupOwner, "Info copied flags");
        check(saved.groupOwnerAddress == info.groupOwnerAddress, "Info address shared by copy");
        check(new WifiP2pInfo((WifiP2pInfo) null).groupOwnerAddress == null, "null Info copy");
        check(info.toString().contains("groupFormed"), "Info text");
    }
    private static void wfdValues() {
        WifiP2pWfdInfo empty = new WifiP2pWfdInfo();
        check(!empty.isEnabled() && !empty.isR2Supported(), "default WFD capabilities");
        equal(0, empty.getControlPort(), "default WFD port");
        equal(0, empty.getMaxThroughput(), "default throughput");
        for (int type = 0; type <= 3; type++) {
            check(empty.setDeviceType(type), "valid WFD type");
            equal(type, empty.getDeviceType(), "WFD type bits");
        }
        WifiP2pWfdInfo value = wfd();
        check(value.isSessionAvailable() && value.isContentProtectionSupported()
                && value.isCoupledSinkSupportedAtSource() && value.isCoupledSinkSupportedAtSink(), "WFD bit setters");
        equal(7236, value.getControlPort(), "WFD control port");
        equal(150, value.getMaxThroughput(), "WFD throughput");
        equal(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK, value.getR2DeviceType(), "R2 type bits");
        WifiP2pWfdInfo copy = new WifiP2pWfdInfo(value);
        equal(value.getDeviceInfo(), copy.getDeviceInfo(), "WFD copy bits");
        equal(value.getR2DeviceInfo(), copy.getR2DeviceInfo(), "WFD copy R2");
        value.setSessionAvailable(false); value.setContentProtectionSupported(false);
        value.setCoupledSinkSupportAtSource(false); value.setCoupledSinkSupportAtSink(false);
        check(!value.isSessionAvailable() && !value.isContentProtectionSupported()
                && !value.isCoupledSinkSupportedAtSource() && !value.isCoupledSinkSupportedAtSink(), "WFD cleared bits");
        check(copy.isSessionAvailable(), "WFD copy independent");
        check(!new WifiP2pWfdInfo((WifiP2pWfdInfo) null).isEnabled(), "null WFD copy");
        check(copy.toString().contains("WFD"), "WFD text");
    }
    /** Java argument-validation cases run on the host; native corpus uses normal values only. */
    private static void invalidArguments() {
        rejects(IllegalStateException.class, () -> new WifiP2pConfig.Builder().build(), "unspecified peer");
        rejects(IllegalStateException.class, () -> peer().setDeviceAddress(null).build(), "reset peer");
        for (String value : new String[]{null, "", "DIRECT-a", "DIRECT-!!", "other", "DIRECT-aa" + "x".repeat(24), "DIRECT-aa" + "\u05d0".repeat(12)})
            rejects(IllegalArgumentException.class, () -> peer().setNetworkName(value), "network-name validation");
        peer().setNetworkName("DIRECT-aa" + "x".repeat(23)); checks++;
        rejects(NullPointerException.class, () -> peer().setPassphrase(null), "null passphrase");
        rejects(IllegalArgumentException.class, () -> peer().setPassphrase(""), "empty passphrase");
        rejects(IllegalArgumentException.class, () -> peer().setPassphrase("x".repeat(129)), "129-character passphrase");
        equal(128, credentials("x".repeat(128)).setPccModeConnectionType(
                WifiP2pConfig.PCC_MODE_CONNECTION_TYPE_R2_ONLY).build().getPassphrase().length(),
                "r4 accepts 128 characters despite stricter prose");
        for (int length : new int[]{1, 7, 64, 128})
            rejects(IllegalArgumentException.class, () -> credentials("x".repeat(length)).build(), "legacy passphrase build boundary");
        for (int length : new int[]{8, 63}) equal(length,
                credentials("\u05d0".repeat(length)).build().getPassphrase().length(), "legacy does not impose ASCII");
        rejects(IllegalStateException.class, () -> peer().setNetworkName("DIRECT-aa").build(), "unpaired name");
        rejects(IllegalStateException.class, () -> peer().setPassphrase("12345678").build(), "unpaired password");
        rejects(IllegalStateException.class, () -> peer().setGroupOperatingBand(1).setGroupOperatingFrequency(2412).build(), "band/frequency conflict");
        rejects(IllegalArgumentException.class, () -> peer().setGroupOperatingFrequency(-1), "negative frequency");
        rejects(IllegalArgumentException.class, () -> peer().setGroupOperatingBand(99), "invalid band");
        rejects(IllegalArgumentException.class, () -> peer().setGroupClientIpProvisioningMode(99), "invalid IP mode");
        rejects(IllegalArgumentException.class, () -> peer().setPccModeConnectionType(99), "invalid PCC mode");
        rejects(NullPointerException.class, () -> peer().setPairingBootstrappingConfig(null), "null pairing");
        for (int method : new int[]{0, 3, 63, 99})
            rejects(IllegalArgumentException.class, () -> new WifiP2pPairingBootstrappingConfig(method, null), "invalid pairing method");
        for (int method : new int[]{WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC,
                WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PINCODE,
                WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_KEYPAD_PASSPHRASE}) {
            rejects(IllegalArgumentException.class, () -> new WifiP2pPairingBootstrappingConfig(method, "x"), "unexpected pairing password");
            new WifiP2pPairingBootstrappingConfig(method, ""); checks++;
        }
        WifiP2pDevice value = device();
        rejects(IllegalArgumentException.class, () -> value.update(null), "null device update");
        rejects(IllegalArgumentException.class, () -> value.update(new WifiP2pDevice()), "mismatched device update");
        WifiP2pDevice nullAddress = device(); nullAddress.deviceAddress = null;
        rejects(IllegalArgumentException.class, () -> value.update(nullAddress), "null update address");
        WifiP2pDeviceList list = new WifiP2pDeviceList();
        rejects(IllegalArgumentException.class, () -> list.get(null), "null lookup");
        rejects(IllegalArgumentException.class, () -> list.get(""), "empty lookup");
        rejects(UnsupportedOperationException.class, () -> list.getDeviceList().add(device()), "unmodifiable devices");
        rejects(UnsupportedOperationException.class, () -> new WifiP2pGroup().getClientList().add(device()), "unmodifiable clients");
        WifiP2pWfdInfo wfd = wfd(); int old = wfd.getDeviceInfo(); int oldR2 = wfd.getR2DeviceInfo();
        check(!wfd.setDeviceType(-1) && !wfd.setDeviceType(4), "invalid WFD types rejected");
        equal(old, wfd.getDeviceInfo(), "invalid WFD type leaves bits");
        check(!wfd.setR2DeviceType(2) && !wfd.setR2DeviceType(4), "invalid R2 types rejected");
        equal(oldR2, wfd.getR2DeviceInfo(), "invalid R2 type leaves bits");
        wfd.setControlPort(-7); wfd.setMaxThroughput(-8);
        equal(-7, wfd.getControlPort(), "WFD port annotation is not validation");
        equal(-8, wfd.getMaxThroughput(), "WFD throughput annotation is not validation");
    }
    public static String digest(byte[] bytes) throws Exception {
        StringBuilder value = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(bytes)) value.append(String.format("%02x", b & 255));
        return value.toString();
    }
    public static <T extends Parcelable> T wire(String name, T value, Parcelable.Creator<T> creator,
            boolean roundtrip) throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            byte[] bytes = parcel.marshall();
            equal(parcel.dataSize(), bytes.length, name + " native extent");
            System.out.println("P2P_VALUES_PARCEL " + name + " bytes=" + bytes.length
                    + " sha256=" + digest(bytes) + " roundtrip=" + roundtrip);
            if (!roundtrip) return null;
            parcel.setDataPosition(0);
            T copy = creator.createFromParcel(parcel);
            check(copy != value && copy.getClass() == value.getClass(), name + " creator identity");
            equal(bytes.length, parcel.dataPosition(), name + " consumed complete ordinary record");
            Parcel again = Parcel.obtain();
            try {
                copy.writeToParcel(again, 0);
                check(Arrays.equals(bytes, again.marshall()), name + " stable ordinary roundtrip");
            } finally { again.recycle(); }
            return copy;
        } finally { parcel.recycle(); }
    }
    private static void normalParcels(boolean r4, boolean bootCoherent) throws Exception {
        wire("config_default", new WifiP2pConfig(), WifiP2pConfig.CREATOR, true);
        wire("config_peer", peer().enablePersistentMode(true).setGroupOperatingFrequency(5180).build(), WifiP2pConfig.CREATOR, true);
        wire("config_unicode", credentials("\u05e1\u05d9\u05e1\u05de\u05d4-12345").build(), WifiP2pConfig.CREATOR, true);
        wire("device_default", new WifiP2pDevice(), WifiP2pDevice.CREATOR, true);
        WifiP2pDevice copied = wire("device_unicode", device(), WifiP2pDevice.CREATOR, true);
        equal(device().deviceName, copied.deviceName, "Unicode/NUL Device name");
        WifiP2pDevice nullable = device(); nullable.deviceName = null; nullable.primaryDeviceType = null; nullable.secondaryDeviceType = null;
        wire("device_null_fields", nullable, WifiP2pDevice.CREATOR, true);
        wire("devicelist_empty", new WifiP2pDeviceList(), WifiP2pDeviceList.CREATOR, bootCoherent);
        WifiP2pDevice first = device();
        WifiP2pDevice second = device(); second.deviceAddress = "02:11:22:33:44:66"; second.deviceName = "second";
        WifiP2pDeviceList populated;
        if (bootCoherent) {
            // A normal two-element DeviceList record, using native public Parcel writers.
            // Null-loader identity was asserted before entering this corpus.
            Parcel listInput = Parcel.obtain();
            try {
                listInput.writeInt(2); listInput.writeParcelable(first, 0); listInput.writeParcelable(second, 0);
                listInput.setDataPosition(0);
                populated = WifiP2pDeviceList.CREATOR.createFromParcel(listInput);
                equal(0, listInput.dataAvail(), "ordinary DeviceList input fully consumed");
            } finally { listInput.recycle(); }
        } else {
            // Source-only setup is separately identified. No child/boot mixed read occurs.
            populated = (WifiP2pDeviceList) Class.forName("gesher.services.WifiP2pValuesExtendedTest")
                    .getMethod("ordinaryList", WifiP2pDevice[].class)
                    .invoke(null, (Object) new WifiP2pDevice[]{first, second});
        }
        equal(2, populated.getDeviceList().size(), "ordinary DeviceList values");
        equal(first.deviceName, populated.get(first.deviceAddress).deviceName, "ordinary DeviceList lookup");
        WifiP2pDeviceList detached = new WifiP2pDeviceList(populated);
        check(detached.get(first.deviceAddress) != populated.get(first.deviceAddress), "public DeviceList copies elements");
        wire("devicelist_public_two", populated, WifiP2pDeviceList.CREATOR, bootCoherent);
        wire("group_default", new WifiP2pGroup(), WifiP2pGroup.CREATOR, true);
        wire("group_default_owner_copy", new WifiP2pGroup(new WifiP2pGroup()), WifiP2pGroup.CREATOR, true);
        wire("info_default", new WifiP2pInfo(), WifiP2pInfo.CREATOR, true);
        for (byte[] address : new byte[][]{{(byte) 192, 0, 2, 7}, {0x20, 0x01, 0x0d, (byte) 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7}}) {
            WifiP2pInfo info = new WifiP2pInfo(); info.groupFormed = true; info.isGroupOwner = address.length == 4;
            info.groupOwnerAddress = InetAddress.getByAddress(address);
            WifiP2pInfo copy = wire(address.length == 4 ? "info_ipv4" : "info_ipv6", info, WifiP2pInfo.CREATOR, true);
            check(Arrays.equals(address, copy.groupOwnerAddress.getAddress()), "Info address bytes");
            equal(info.isGroupOwner, copy.isGroupOwner, "Info owner flag");
        }
        wire("wfd_default", new WifiP2pWfdInfo(), WifiP2pWfdInfo.CREATOR, true);
        wire("wfd_configured", wfd(), WifiP2pWfdInfo.CREATOR, true);
        if (r4) {
            WifiP2pPairingBootstrappingConfig pairing = new WifiP2pPairingBootstrappingConfig(
                    WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PASSPHRASE, "\u05d0\u05d1\ud83d\udd11\u0000x");
            wire("pairing_unicode", pairing, WifiP2pPairingBootstrappingConfig.CREATOR, true);
            wire("pairing_opportunistic", new WifiP2pPairingBootstrappingConfig(
                    WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_OPPORTUNISTIC, null), WifiP2pPairingBootstrappingConfig.CREATOR, true);
            wire("pairing_pin", new WifiP2pPairingBootstrappingConfig(
                    WifiP2pPairingBootstrappingConfig.PAIRING_BOOTSTRAPPING_METHOD_DISPLAY_PINCODE, "12345678"), WifiP2pPairingBootstrappingConfig.CREATOR, true);
            wire("config_pairing", peer().setPairingBootstrappingConfig(pairing)
                    .setAuthorizeConnectionFromPeerEnabled(true).build(), WifiP2pConfig.CREATOR, true);
        }
    }
    public static void run(boolean nativeParcel, boolean r4Profile, boolean bootCoherentP2p) throws Exception {
        checks = 0;
        if (r4Profile) constants();
        configValues(r4Profile); deviceValues(r4Profile); groupAndInfo(r4Profile); wfdValues();
        if (!nativeParcel) invalidArguments();
        if (nativeParcel) normalParcels(r4Profile, bootCoherentP2p);
        System.out.println("WifiP2pValuesContractTest passed: " + checks + " checks; parcel=" + nativeParcel
                + "; profile=" + (r4Profile ? "android16-r4" : "stock-api36")
                + "; bootCoherentP2p=" + bootCoherentP2p);
    }
    public static void baselineControl() { new WifiP2pConfig.Builder(); }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        ClassLoader boot = Parcel.class.getClassLoader();
        if (boot != null && !boot.getClass().getName().endsWith("BootClassLoader"))
            throw new AssertionError("Native framework boot provider required");
        for (Class<?> type : new Class<?>[]{WifiP2pConfig.class, WifiP2pConfig.Builder.class,
                WifiP2pPairingBootstrappingConfig.class, WifiP2pDevice.class, WifiP2pWfdInfo.class,
                WifiP2pDeviceList.class, WifiP2pGroup.class, WifiP2pInfo.class, WpsInfo.class, MacAddress.class})
            if (type.getClassLoader() != boot) throw new AssertionError("Non-boot P2P graph: " + type);
        check(Class.forName("android.net.wifi.p2p.WifiP2pDevice", false, boot) == WifiP2pDevice.class,
                "Parcel null loader resolves production Device");
        System.out.println("P2P_VALUES_PROVIDERS coherent production boot graph; Parcel=null-loader Device identity verified");
        run(true, true, true);
        Class.forName("gesher.services.WifiP2pValuesExtendedTest").getMethod("run", boolean.class, boolean.class).invoke(null, true, true);
    }
}
