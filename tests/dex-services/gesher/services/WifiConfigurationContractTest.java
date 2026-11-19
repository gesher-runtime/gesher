/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.IpConfiguration;
import android.net.MacAddress;
import android.net.ProxyInfo;
import android.net.Uri;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiEnterpriseConfig;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.BitSet;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/**
 * SDK36 callers shared by the host, Windows ART, and Android reference. Hidden
 * reflection is confined to producer state and supporting values, never a service.
 */
public final class WifiConfigurationContractTest {
    private static int checks;
    private static final int SENTINEL = 0x56c0ff1;
    private interface Action { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> expected, Action action, String message)
            throws Exception {
        try { action.run(); } catch (Throwable failure) {
            if (failure instanceof InvocationTargetException) failure = ((InvocationTargetException) failure).getCause();
            if (!expected.isInstance(failure)) throw new AssertionError(message + ": expected " + expected.getSimpleName(), failure);
            checks++;
            return;
        }
        throw new AssertionError(message + ": did not throw " + expected.getSimpleName());
    }
    private static Object invoke(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }
    private static void constant(Class<?> owner, String name, Object sdkValue) throws Exception {
        // javac inlines sdkValue from the official SDK; reflection reads the shipped field.
        check(Objects.equals(sdkValue, owner.getField(name).get(null)), "Runtime constant " + owner.getSimpleName() + "." + name);
    }
    private static void constants() throws Exception {
        constant(WifiConfiguration.class, "RANDOMIZATION_AUTO", WifiConfiguration.RANDOMIZATION_AUTO);
        constant(WifiConfiguration.class, "RANDOMIZATION_NONE", WifiConfiguration.RANDOMIZATION_NONE);
        constant(WifiConfiguration.class, "RANDOMIZATION_NON_PERSISTENT", WifiConfiguration.RANDOMIZATION_NON_PERSISTENT);
        constant(WifiConfiguration.class, "RANDOMIZATION_PERSISTENT", WifiConfiguration.RANDOMIZATION_PERSISTENT);
        constant(WifiConfiguration.class, "SECURITY_TYPE_DPP", WifiConfiguration.SECURITY_TYPE_DPP);
        constant(WifiConfiguration.class, "SECURITY_TYPE_EAP", WifiConfiguration.SECURITY_TYPE_EAP);
        constant(WifiConfiguration.class, "SECURITY_TYPE_EAP_SUITE_B", WifiConfiguration.SECURITY_TYPE_EAP_SUITE_B);
        constant(WifiConfiguration.class, "SECURITY_TYPE_EAP_WPA3_ENTERPRISE", WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE);
        constant(WifiConfiguration.class, "SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT", WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT);
        constant(WifiConfiguration.class, "SECURITY_TYPE_OPEN", WifiConfiguration.SECURITY_TYPE_OPEN);
        constant(WifiConfiguration.class, "SECURITY_TYPE_OWE", WifiConfiguration.SECURITY_TYPE_OWE);
        constant(WifiConfiguration.class, "SECURITY_TYPE_PSK", WifiConfiguration.SECURITY_TYPE_PSK);
        constant(WifiConfiguration.class, "SECURITY_TYPE_SAE", WifiConfiguration.SECURITY_TYPE_SAE);
        constant(WifiConfiguration.class, "SECURITY_TYPE_WAPI_CERT", WifiConfiguration.SECURITY_TYPE_WAPI_CERT);
        constant(WifiConfiguration.class, "SECURITY_TYPE_WAPI_PSK", WifiConfiguration.SECURITY_TYPE_WAPI_PSK);
        constant(WifiConfiguration.class, "SECURITY_TYPE_WEP", WifiConfiguration.SECURITY_TYPE_WEP);
        constant(WifiConfiguration.KeyMgmt.class, "DPP", WifiConfiguration.KeyMgmt.DPP);
        constant(WifiConfiguration.KeyMgmt.class, "FILS_SHA256", WifiConfiguration.KeyMgmt.FILS_SHA256);
        constant(WifiConfiguration.KeyMgmt.class, "FILS_SHA384", WifiConfiguration.KeyMgmt.FILS_SHA384);
        constant(WifiConfiguration.KeyMgmt.class, "FT_EAP", WifiConfiguration.KeyMgmt.FT_EAP);
        constant(WifiConfiguration.KeyMgmt.class, "FT_PSK", WifiConfiguration.KeyMgmt.FT_PSK);
        constant(WifiConfiguration.KeyMgmt.class, "IEEE8021X", WifiConfiguration.KeyMgmt.IEEE8021X);
        constant(WifiConfiguration.KeyMgmt.class, "NONE", WifiConfiguration.KeyMgmt.NONE);
        constant(WifiConfiguration.KeyMgmt.class, "OSEN", WifiConfiguration.KeyMgmt.OSEN);
        constant(WifiConfiguration.KeyMgmt.class, "OWE", WifiConfiguration.KeyMgmt.OWE);
        constant(WifiConfiguration.KeyMgmt.class, "SAE", WifiConfiguration.KeyMgmt.SAE);
        constant(WifiConfiguration.KeyMgmt.class, "SUITE_B_192", WifiConfiguration.KeyMgmt.SUITE_B_192);
        constant(WifiConfiguration.KeyMgmt.class, "WAPI_CERT", WifiConfiguration.KeyMgmt.WAPI_CERT);
        constant(WifiConfiguration.KeyMgmt.class, "WAPI_PSK", WifiConfiguration.KeyMgmt.WAPI_PSK);
        constant(WifiConfiguration.KeyMgmt.class, "WPA2_PSK", WifiConfiguration.KeyMgmt.WPA2_PSK);
        constant(WifiConfiguration.KeyMgmt.class, "WPA_EAP", WifiConfiguration.KeyMgmt.WPA_EAP);
        constant(WifiConfiguration.KeyMgmt.class, "WPA_EAP_SHA256", WifiConfiguration.KeyMgmt.WPA_EAP_SHA256);
        constant(WifiConfiguration.KeyMgmt.class, "WPA_PSK", WifiConfiguration.KeyMgmt.WPA_PSK);
        constant(WifiConfiguration.KeyMgmt.class, "WPA_PSK_SHA256", WifiConfiguration.KeyMgmt.WPA_PSK_SHA256);
        constant(WifiConfiguration.KeyMgmt.class, "varName", WifiConfiguration.KeyMgmt.varName);
        constant(WifiConfiguration.GroupMgmtCipher.class, "BIP_CMAC_256", WifiConfiguration.GroupMgmtCipher.BIP_CMAC_256);
        constant(WifiConfiguration.GroupMgmtCipher.class, "BIP_GMAC_128", WifiConfiguration.GroupMgmtCipher.BIP_GMAC_128);
        constant(WifiConfiguration.GroupMgmtCipher.class, "BIP_GMAC_256", WifiConfiguration.GroupMgmtCipher.BIP_GMAC_256);
        constant(WifiConfiguration.Status.class, "CURRENT", WifiConfiguration.Status.CURRENT);
        constant(WifiConfiguration.Status.class, "DISABLED", WifiConfiguration.Status.DISABLED);
        constant(WifiConfiguration.Status.class, "ENABLED", WifiConfiguration.Status.ENABLED);
        constant(WifiConfiguration.Protocol.class, "RSN", WifiConfiguration.Protocol.RSN);
        constant(WifiConfiguration.Protocol.class, "WAPI", WifiConfiguration.Protocol.WAPI);
        constant(WifiConfiguration.Protocol.class, "WPA", WifiConfiguration.Protocol.WPA);
        constant(WifiConfiguration.Protocol.class, "varName", WifiConfiguration.Protocol.varName);
        constant(WifiConfiguration.GroupCipher.class, "CCMP", WifiConfiguration.GroupCipher.CCMP);
        constant(WifiConfiguration.GroupCipher.class, "GCMP_128", WifiConfiguration.GroupCipher.GCMP_128);
        constant(WifiConfiguration.GroupCipher.class, "GCMP_256", WifiConfiguration.GroupCipher.GCMP_256);
        constant(WifiConfiguration.GroupCipher.class, "SMS4", WifiConfiguration.GroupCipher.SMS4);
        constant(WifiConfiguration.GroupCipher.class, "TKIP", WifiConfiguration.GroupCipher.TKIP);
        constant(WifiConfiguration.GroupCipher.class, "WEP104", WifiConfiguration.GroupCipher.WEP104);
        constant(WifiConfiguration.GroupCipher.class, "WEP40", WifiConfiguration.GroupCipher.WEP40);
        constant(WifiConfiguration.GroupCipher.class, "varName", WifiConfiguration.GroupCipher.varName);
        constant(WifiConfiguration.PairwiseCipher.class, "CCMP", WifiConfiguration.PairwiseCipher.CCMP);
        constant(WifiConfiguration.PairwiseCipher.class, "GCMP_128", WifiConfiguration.PairwiseCipher.GCMP_128);
        constant(WifiConfiguration.PairwiseCipher.class, "GCMP_256", WifiConfiguration.PairwiseCipher.GCMP_256);
        constant(WifiConfiguration.PairwiseCipher.class, "NONE", WifiConfiguration.PairwiseCipher.NONE);
        constant(WifiConfiguration.PairwiseCipher.class, "SMS4", WifiConfiguration.PairwiseCipher.SMS4);
        constant(WifiConfiguration.PairwiseCipher.class, "TKIP", WifiConfiguration.PairwiseCipher.TKIP);
        constant(WifiConfiguration.PairwiseCipher.class, "varName", WifiConfiguration.PairwiseCipher.varName);
        constant(WifiConfiguration.AuthAlgorithm.class, "LEAP", WifiConfiguration.AuthAlgorithm.LEAP);
        constant(WifiConfiguration.AuthAlgorithm.class, "OPEN", WifiConfiguration.AuthAlgorithm.OPEN);
        constant(WifiConfiguration.AuthAlgorithm.class, "SAE", WifiConfiguration.AuthAlgorithm.SAE);
        constant(WifiConfiguration.AuthAlgorithm.class, "SHARED", WifiConfiguration.AuthAlgorithm.SHARED);
        constant(WifiConfiguration.AuthAlgorithm.class, "varName", WifiConfiguration.AuthAlgorithm.varName);
        constant(WifiEnterpriseConfig.class, "EXTRA_WAPI_AS_CERTIFICATE_DATA", WifiEnterpriseConfig.EXTRA_WAPI_AS_CERTIFICATE_DATA);
        constant(WifiEnterpriseConfig.class, "EXTRA_WAPI_AS_CERTIFICATE_NAME", WifiEnterpriseConfig.EXTRA_WAPI_AS_CERTIFICATE_NAME);
        constant(WifiEnterpriseConfig.class, "EXTRA_WAPI_USER_CERTIFICATE_DATA", WifiEnterpriseConfig.EXTRA_WAPI_USER_CERTIFICATE_DATA);
        constant(WifiEnterpriseConfig.class, "EXTRA_WAPI_USER_CERTIFICATE_NAME", WifiEnterpriseConfig.EXTRA_WAPI_USER_CERTIFICATE_NAME);
        constant(WifiEnterpriseConfig.class, "TLS_V1_0", WifiEnterpriseConfig.TLS_V1_0);
        constant(WifiEnterpriseConfig.class, "TLS_V1_1", WifiEnterpriseConfig.TLS_V1_1);
        constant(WifiEnterpriseConfig.class, "TLS_V1_2", WifiEnterpriseConfig.TLS_V1_2);
        constant(WifiEnterpriseConfig.class, "TLS_V1_3", WifiEnterpriseConfig.TLS_V1_3);
        constant(WifiEnterpriseConfig.class, "WAPI_AS_CERTIFICATE", WifiEnterpriseConfig.WAPI_AS_CERTIFICATE);
        constant(WifiEnterpriseConfig.class, "WAPI_USER_CERTIFICATE", WifiEnterpriseConfig.WAPI_USER_CERTIFICATE);
        constant(WifiEnterpriseConfig.Phase2.class, "AKA", WifiEnterpriseConfig.Phase2.AKA);
        constant(WifiEnterpriseConfig.Phase2.class, "AKA_PRIME", WifiEnterpriseConfig.Phase2.AKA_PRIME);
        constant(WifiEnterpriseConfig.Phase2.class, "GTC", WifiEnterpriseConfig.Phase2.GTC);
        constant(WifiEnterpriseConfig.Phase2.class, "MSCHAP", WifiEnterpriseConfig.Phase2.MSCHAP);
        constant(WifiEnterpriseConfig.Phase2.class, "MSCHAPV2", WifiEnterpriseConfig.Phase2.MSCHAPV2);
        constant(WifiEnterpriseConfig.Phase2.class, "NONE", WifiEnterpriseConfig.Phase2.NONE);
        constant(WifiEnterpriseConfig.Phase2.class, "PAP", WifiEnterpriseConfig.Phase2.PAP);
        constant(WifiEnterpriseConfig.Phase2.class, "SIM", WifiEnterpriseConfig.Phase2.SIM);
        constant(WifiEnterpriseConfig.Eap.class, "AKA", WifiEnterpriseConfig.Eap.AKA);
        constant(WifiEnterpriseConfig.Eap.class, "AKA_PRIME", WifiEnterpriseConfig.Eap.AKA_PRIME);
        constant(WifiEnterpriseConfig.Eap.class, "NONE", WifiEnterpriseConfig.Eap.NONE);
        constant(WifiEnterpriseConfig.Eap.class, "PEAP", WifiEnterpriseConfig.Eap.PEAP);
        constant(WifiEnterpriseConfig.Eap.class, "PWD", WifiEnterpriseConfig.Eap.PWD);
        constant(WifiEnterpriseConfig.Eap.class, "SIM", WifiEnterpriseConfig.Eap.SIM);
        constant(WifiEnterpriseConfig.Eap.class, "TLS", WifiEnterpriseConfig.Eap.TLS);
        constant(WifiEnterpriseConfig.Eap.class, "TTLS", WifiEnterpriseConfig.Eap.TTLS);
        constant(WifiEnterpriseConfig.Eap.class, "UNAUTH_TLS", WifiEnterpriseConfig.Eap.UNAUTH_TLS);
        constant(WifiEnterpriseConfig.Eap.class, "WAPI_CERT", WifiEnterpriseConfig.Eap.WAPI_CERT);
        check(Arrays.equals(WifiConfiguration.KeyMgmt.strings, new String[]{"NONE", "WPA_PSK", "WPA_EAP", "IEEE8021X",
                "WPA2_PSK", "OSEN", "FT_PSK", "FT_EAP", "SAE", "OWE", "SUITE_B_192", "WPA_PSK_SHA256",
                "WPA_EAP_SHA256", "WAPI_PSK", "WAPI_CERT", "FILS_SHA256", "FILS_SHA384", "DPP"}), "Key management text table");
        check(Arrays.equals(WifiConfiguration.Protocol.strings, new String[]{"WPA", "RSN", "OSEN", "WAPI"}), "Protocol text table");
        check(Arrays.equals(WifiConfiguration.AuthAlgorithm.strings, new String[]{"OPEN", "SHARED", "LEAP", "SAE"}), "Authentication text table");
        check(Arrays.equals(WifiConfiguration.PairwiseCipher.strings,
                new String[]{"NONE", "TKIP", "CCMP", "GCMP_256", "SMS4", "GCMP_128"}), "Pairwise text table");
        check(Arrays.equals(WifiConfiguration.GroupCipher.strings,
                new String[]{"WEP40", "WEP104", "TKIP", "CCMP", "GTK_NOT_USED", "GCMP_256", "SMS4", "GCMP_128"}), "Group cipher text table");
    }
    private static BitSet bits(int... positions) {
        BitSet value = new BitSet();
        for (int position : positions) value.set(position);
        return value;
    }
    private static BitSet[] masks(WifiConfiguration value) {
        return new BitSet[]{value.allowedKeyManagement, value.allowedProtocols, value.allowedAuthAlgorithms,
                value.allowedPairwiseCiphers, value.allowedGroupCiphers, value.allowedGroupManagementCiphers, value.allowedSuiteBCiphers};
    }
    private static void defaultsAndCopy() throws Exception {
        WifiConfiguration value = new WifiConfiguration();
        // This SDK call site fails on the original production class: no copy constructor.
        WifiConfiguration emptyCopy = new WifiConfiguration(value);
        check(emptyCopy != value && emptyCopy.networkId == -1, "SDK copy constructor links");
        check(value.networkId == -1 && value.status == WifiConfiguration.Status.CURRENT, "Default network identity and status");
        check(value.SSID == null && value.BSSID == null && value.FQDN == null
                && value.providerFriendlyName == null && value.preSharedKey == null, "Absent network strings");
        check(value.roamingConsortiumIds.length == 0 && value.wepKeys.length == 4
                && Arrays.equals(value.wepKeys, new String[4]), "Default roaming and WEP arrays");
        for (BitSet mask : masks(value)) check(mask != null && mask.isEmpty(), "Default empty security mask");
        check(value.enterpriseConfig != null && value.enterpriseConfig.getEapMethod() == WifiEnterpriseConfig.Eap.NONE, "Default enterprise state");
        check(!value.hiddenSSID && !value.isHomeProviderNetwork && value.priority == 0 && value.wepTxKeyIndex == 0, "Default network scalars");
        check(!value.isPasspoint() && !value.isDppConfigurator() && value.describeContents() == 0, "No implied Passpoint, DPP or file descriptors");
        check(value.getMacRandomizationSetting() == WifiConfiguration.RANDOMIZATION_AUTO
                && value.getRandomizedMacAddress().equals(MacAddress.fromString("02:00:00:00:00:00")), "Unassigned randomized address");
        check("nullNONE".equals(value.getKey()), "Unassigned network key");
        for (int setting : new int[]{0, 1, 2, 3, -1, 99}) {
            value.setMacRandomizationSetting(setting);
            check(value.getMacRandomizationSetting() == setting, "Randomization value storage " + setting);
        }
        value.SSID = "\"Office\"";
        value.BSSID = "02:11:22:33:44:55";
        value.FQDN = "wifi.example.test";
        value.providerFriendlyName = "Example";
        value.isHomeProviderNetwork = true;
        value.networkId = 73;
        value.status = WifiConfiguration.Status.DISABLED;
        value.priority = 17;
        value.hiddenSSID = true;
        value.preSharedKey = "do-not-display-configuration-secret";
        value.roamingConsortiumIds = new long[]{0x112233L, 0xffffffffffL};
        value.wepKeys[2] = "0011223344";
        value.wepTxKeyIndex = 2;
        value.setSecurityParams(WifiConfiguration.SECURITY_TYPE_EAP);
        value.enterpriseConfig.setEapMethod(WifiEnterpriseConfig.Eap.TTLS);
        value.enterpriseConfig.setPhase2Method(WifiEnterpriseConfig.Phase2.MSCHAPV2);
        value.enterpriseConfig.setIdentity("identity@example.test");
        value.setHttpProxy(ProxyInfo.buildDirectProxy("proxy.example.test", 8123));
        WifiConfiguration copy = new WifiConfiguration(value);
        check(copy.networkId == 73 && copy.status == value.status && copy.priority == 17
                && copy.hiddenSSID && copy.isHomeProviderNetwork && copy.wepTxKeyIndex == 2, "Copy network scalars");
        check(Objects.equals(copy.SSID, value.SSID) && Objects.equals(copy.BSSID, value.BSSID)
                && Objects.equals(copy.FQDN, value.FQDN) && Objects.equals(copy.providerFriendlyName, value.providerFriendlyName)
                && Objects.equals(copy.preSharedKey, value.preSharedKey), "Copy network strings");
        check(Arrays.equals(copy.roamingConsortiumIds, value.roamingConsortiumIds)
                && copy.roamingConsortiumIds != value.roamingConsortiumIds, "Copy owns roaming array");
        check(Arrays.equals(copy.wepKeys, value.wepKeys) && copy.wepKeys != value.wepKeys, "Copy owns WEP array");
        BitSet[] originalMasks = masks(value), copiedMasks = masks(copy);
        for (int i = 0; i < originalMasks.length; i++) {
            check(copiedMasks[i].equals(originalMasks[i]) && copiedMasks[i] != originalMasks[i], "Copy owns security mask " + i);
            originalMasks[i].set(42);
            check(!copiedMasks[i].get(42), "Mutation does not reach copied mask " + i);
        }
        check(copy.enterpriseConfig != value.enterpriseConfig
                && "identity@example.test".equals(copy.enterpriseConfig.getIdentity()), "Copy owns enterprise state");
        value.enterpriseConfig.setIdentity("changed");
        value.roamingConsortiumIds[0] = 7;
        value.wepKeys[2] = "changed";
        value.setHttpProxy(null);
        check("identity@example.test".equals(copy.enterpriseConfig.getIdentity()) && copy.roamingConsortiumIds[0] == 0x112233L
                && "0011223344".equals(copy.wepKeys[2]) && copy.getHttpProxy().getPort() == 8123, "Copied nested values remain independent");
        check(!copy.toString().contains(value.preSharedKey), "Diagnostic string conceals pre-shared key");
        WifiConfiguration nullCopy = new WifiConfiguration((WifiConfiguration) null);
        check(nullCopy.networkId == 0 && nullCopy.enterpriseConfig == null && nullCopy.allowedKeyManagement == null
                && nullCopy.wepKeys == null, "Null source retains pinned copy-constructor semantics");
        WifiConfiguration malformed = new WifiConfiguration();
        malformed.wepKeys = new String[1];
        expect(ArrayIndexOutOfBoundsException.class, () -> new WifiConfiguration(malformed), "Malformed WEP array is not silently repaired");
        WifiConfiguration producer = new WifiConfiguration();
        MacAddress address = MacAddress.fromString("02:21:43:65:87:a9");
        invoke(producer, "setRandomizedMacAddress", new Class<?>[]{MacAddress.class}, address);
        check(producer.getRandomizedMacAddress().equals(address), "Producer randomized address value");
        byte[] keyBytes = WifiConfigurationCertificates.key().getEncoded();
        byte[] expectedKey = keyBytes.clone();
        invoke(producer, "setDppConfigurator", new Class<?>[]{byte[].class}, keyBytes);
        keyBytes[0] ^= 1;
        check(producer.isDppConfigurator() && Arrays.equals(expectedKey,
                (byte[]) invoke(producer, "getDppPrivateEcKey", new Class<?>[0])), "DPP producer owns supplied key bytes");
        byte[] exposed = (byte[]) invoke(producer, "getDppPrivateEcKey", new Class<?>[0]);
        exposed[0] ^= 1;
        WifiConfiguration producerCopy = new WifiConfiguration(producer);
        check(producerCopy.isDppConfigurator() && producerCopy.getRandomizedMacAddress().equals(address)
                && Arrays.equals(expectedKey, (byte[]) invoke(producerCopy, "getDppPrivateEcKey", new Class<?>[0])),
                "DPP getter and configuration copy retain independent byte ownership");
        invoke(producer, "setDppConfigurator", new Class<?>[]{byte[].class}, (Object) null);
        check(producer.isDppConfigurator(), "Null DPP producer update preserves prior value");
    }
    private static void security() throws Exception {
        // Pinned AOSP suite masks. Rows 10-12 also exercise accepted integer values
        // of the public setter whose symbolic names are outside the public SDK.
        int[][][] expected = {
            {{0}, {0,1}, {}, {}, {}, {}, {}},
            {{0}, {1}, {0,1}, {1,2}, {0,1,2,3}, {}, {}},
            {{1}, {0,1}, {}, {1,2}, {0,1,2,3}, {}, {}},
            {{2,3}, {0,1}, {}, {1,2,3}, {2,3,5}, {}, {}},
            {{8}, {1}, {}, {2,3,5}, {3,5,7}, {}, {}},
            {{2,3,10}, {1}, {}, {3,5}, {5,7}, {2}, {}},
            {{9}, {1}, {}, {2,3,5}, {3,5,7}, {}, {}},
            {{13}, {3}, {}, {4}, {6}, {}, {}},
            {{14}, {3}, {}, {4}, {6}, {}, {}},
            {{2,3}, {1}, {}, {2,3}, {3,5}, {}, {}},
            {{5}, {2}, {}, {1,2}, {2,3}, {}, {}},
            {{2,3}, {1}, {}, {2,3}, {3,5}, {}, {}},
            {{2,3}, {1}, {}, {2,3}, {3,5}, {}, {}},
            {{17}, {1}, {}, {2,3,5}, {3,5,7}, {}, {}}
        };
        WifiConfiguration value = new WifiConfiguration();
        for (int type = 0; type < expected.length; type++) {
            for (BitSet mask : masks(value)) mask.set(43);
            value.setSecurityParams(type);
            BitSet[] actual = masks(value);
            for (int i = 0; i < actual.length; i++) check(actual[i].equals(bits(expected[type][i])),
                    "Security " + type + " mask " + i + " was " + actual[i]);
        }
        for (int invalid : new int[]{-1, 14, Integer.MAX_VALUE}) {
            value.setSecurityParams(WifiConfiguration.SECURITY_TYPE_PSK);
            BitSet before = (BitSet) value.allowedKeyManagement.clone();
            expect(IllegalArgumentException.class, () -> value.setSecurityParams(invalid), "Unknown security type " + invalid);
            check(value.allowedKeyManagement.equals(before), "Rejected type retains prior legacy mask");
        }
        value.SSID = "\"Office\"";
        value.setSecurityParams(WifiConfiguration.SECURITY_TYPE_PSK);
        check("\"Office\"WPA_PSK".equals(value.getKey()), "Quoted SSID and security key");
        Locale saved = Locale.getDefault();
        try {
            Locale.setDefault(Locale.US);
            value.SSID = "ABCDEF";
            check("abcdefWPA_PSK".equals(value.getKey()), "Unquoted hexadecimal SSID normalization");
        } finally { Locale.setDefault(saved); }
        value.setSecurityParams(WifiConfiguration.SECURITY_TYPE_WEP);
        value.wepKeys[1] = "0011223344";
        value.wepTxKeyIndex = 1;
        check(value.getKey().endsWith("WEP"), "Selected WEP key determines key identity");
        value.wepTxKeyIndex = 4;
        check(value.getKey().endsWith("NONE"), "Out-of-range WEP index does not select a key");
        value.FQDN = "example.test";
        value.providerFriendlyName = "Example";
        value.enterpriseConfig.setEapMethod(WifiEnterpriseConfig.Eap.TTLS);
        check(!value.isPasspoint(), "Metadata alone does not invent Passpoint identity");
        invoke(value, "setPasspointUniqueId", new Class<?>[]{String.class}, "provider-unique-id");
        check(value.isPasspoint() && "provider-unique-id".equals(value.getKey()), "Real producer Passpoint identity");
        value.providerFriendlyName = "";
        check(!value.isPasspoint(), "Incomplete Passpoint provider remains invalid");
        invoke(value, "setPasspointUniqueId", new Class<?>[]{String.class}, (Object) null);
        WifiConfiguration.class.getField("shared").setBoolean(value, false);
        WifiConfiguration.class.getField("creatorUid").setInt(value, 1012345);
        check(value.getKey().endsWith("-10"), "Non-shared key retains creator user identity");
    }
    private static void proxies() throws Exception {
        WifiConfiguration value = new WifiConfiguration();
        check(value.getHttpProxy() != null && value.getHttpProxy().getHost() == null
                && value.getHttpProxy().getPort() == 0, "Default UNASSIGNED proxy state");
        ProxyInfo proxy = ProxyInfo.buildDirectProxy("proxy.example.test", 8080, Arrays.asList("localhost", "*.test"));
        value.setHttpProxy(proxy);
        check(value.getHttpProxy().equals(proxy) && value.getHttpProxy() != proxy
                && value.getHttpProxy() != value.getHttpProxy(), "Direct proxy copies");
        ProxyInfo pac = ProxyInfo.buildPacProxy(Uri.parse("https://proxy.example.test/proxy.pac"), 3128);
        value.setHttpProxy(pac);
        check(value.getHttpProxy().equals(pac), "PAC proxy URI and port");
        value.setHttpProxy(null);
        check(value.getHttpProxy() == null, "Explicit NONE proxy state");
        IpConfiguration ip = new IpConfiguration.Builder().setHttpProxy(proxy).build();
        value.setIpConfiguration(ip);
        check(value.getHttpProxy().equals(proxy), "Public IP configuration assignment");
        value.setIpConfiguration(null);
        check(value.getHttpProxy() != null && value.getHttpProxy().getHost() == null, "Null IP configuration restores UNASSIGNED state");
    }
    private static void enterpriseFields() throws Exception {
        WifiEnterpriseConfig value = new WifiEnterpriseConfig();
        check(value.getEapMethod() == WifiEnterpriseConfig.Eap.NONE && value.getPhase2Method() == WifiEnterpriseConfig.Phase2.NONE
                && value.getMinimumTlsVersion() == WifiEnterpriseConfig.TLS_V1_0, "Enterprise defaults");
        check("".equals(value.getIdentity()) && "".equals(value.getPassword()) && "".equals(value.getAnonymousIdentity())
                && "".equals(value.getRealm()) && "".equals(value.getPlmn()) && "".equals(value.getSubjectMatch())
                && "".equals(value.getAltSubjectMatch()) && "".equals(value.getDomainSuffixMatch()), "Absent enterprise strings");
        check(!value.hasCaCertificate() && !value.isTrustOnFirstUseEnabled() && value.getClientKeyPairAlias() == null
                && value.getDecoratedIdentityPrefix() == null && value.describeContents() == 0, "Absent enterprise optional state");
        expect(IllegalStateException.class, value::isServerCertValidationEnabled, "Unset EAP has no certificate validation");
        expect(NullPointerException.class, () -> new WifiEnterpriseConfig(null), "Null enterprise copy source");
        for (int eap = 0; eap <= 8; eap++) {
            value.setPhase2Method(WifiEnterpriseConfig.Phase2.MSCHAPV2);
            value.setEapMethod(eap);
            check(value.getEapMethod() == eap, "EAP setter " + eap);
            boolean resets = eap == WifiEnterpriseConfig.Eap.TLS || eap == WifiEnterpriseConfig.Eap.UNAUTH_TLS
                    || eap == WifiEnterpriseConfig.Eap.WAPI_CERT;
            check(value.getPhase2Method() == (resets ? WifiEnterpriseConfig.Phase2.NONE : WifiEnterpriseConfig.Phase2.MSCHAPV2),
                    "EAP phase2 transition " + eap);
            check(value.isEapMethodServerCertUsed() == (eap == 0 || eap == 1 || eap == 2 || eap == 7), "EAP certificate requirement " + eap);
            check(value.isAuthenticationSimBased() == (eap == 4 || eap == 5 || eap == 6), "EAP SIM classification " + eap);
        }
        for (int invalid : new int[]{-1, 9, Integer.MAX_VALUE}) {
            expect(IllegalArgumentException.class, () -> value.setEapMethod(invalid), "Invalid EAP method " + invalid);
            check(value.getEapMethod() == WifiEnterpriseConfig.Eap.WAPI_CERT, "Invalid EAP preserves method");
        }
        value.setEapMethod(WifiEnterpriseConfig.Eap.PEAP);
        for (int phase = 0; phase <= 7; phase++) {
            value.setPhase2Method(phase);
            check(value.getPhase2Method() == phase && value.isAuthenticationSimBased() == (phase >= 5), "PEAP inner authentication " + phase);
        }
        expect(IllegalArgumentException.class, () -> value.setPhase2Method(-1), "Negative phase2");
        expect(IllegalArgumentException.class, () -> value.setPhase2Method(8), "Unknown phase2");
        check(value.getPhase2Method() == 7, "Rejected phase2 preserves previous state");
        for (int tls = 0; tls <= 3; tls++) {
            value.setMinimumTlsVersion(tls);
            check(value.getMinimumTlsVersion() == tls, "Minimum TLS " + tls);
        }
        expect(IllegalArgumentException.class, () -> value.setMinimumTlsVersion(-1), "Negative TLS version");
        expect(IllegalArgumentException.class, () -> value.setMinimumTlsVersion(4), "Unknown TLS version");
        check(value.getMinimumTlsVersion() == WifiEnterpriseConfig.TLS_V1_3, "Rejected TLS preserves previous state");
        String[] setters = {"setIdentity", "setPassword", "setAnonymousIdentity", "setRealm", "setPlmn",
                "setSubjectMatch", "setAltSubjectMatch", "setDomainSuffixMatch"};
        String[] getters = {"getIdentity", "getPassword", "getAnonymousIdentity", "getRealm", "getPlmn",
                "getSubjectMatch", "getAltSubjectMatch", "getDomainSuffixMatch"};
        int[] limits = {256, 256, 1024, 256, 16, 256, 1024, 256};
        for (int i = 0; i < setters.length; i++) {
            Method setter = WifiEnterpriseConfig.class.getMethod(setters[i], String.class);
            Method getter = WifiEnterpriseConfig.class.getMethod(getters[i]);
            String text = "value-\"quoted\"-" + i;
            setter.invoke(value, text);
            check(text.equals(getter.invoke(value)), "Enterprise string retains quoting " + setters[i]);
            setter.invoke(value, repeat('x', limits[i] + 1));
            check(text.equals(getter.invoke(value)), "Oversized enterprise value ignored " + setters[i]);
            setter.invoke(value, (Object) null);
            check("".equals(getter.invoke(value)), "Null enterprise value clears " + setters[i]);
        }
        value.setClientKeyPairAlias("key-alias");
        expect(IllegalArgumentException.class, () -> value.setClientKeyPairAlias(repeat('x', 257)), "Oversized key alias");
        check("key-alias".equals(value.getClientKeyPairAlias()), "Invalid alias preserves previous state");
        value.setClientKeyPairAlias(null);
        check(value.getClientKeyPairAlias() == null, "Null key alias clears");
        value.setDecoratedIdentityPrefix("realm!");
        check("realm!".equals(value.getDecoratedIdentityPrefix()), "Decorated identity prefix");
        expect(IllegalArgumentException.class, () -> value.setDecoratedIdentityPrefix("realm"), "Decoration requires terminator");
        check("realm!".equals(value.getDecoratedIdentityPrefix()), "Invalid decoration preserves previous state");
        value.setDecoratedIdentityPrefix("");
        check(value.getDecoratedIdentityPrefix() == null, "Empty decoration clears");
        value.enableTrustOnFirstUse(true);
        check(value.isTrustOnFirstUseEnabled(), "TOFU value enabled without implying trust success");
        value.enableTrustOnFirstUse(false);
        check(!value.isTrustOnFirstUseEnabled(), "TOFU value cleared");
        value.setPassword("do-not-display-enterprise-secret");
        check(!value.toString().contains(value.getPassword()), "Diagnostic string conceals enterprise password");
    }
    private static String repeat(char ch, int count) {
        char[] chars = new char[count];
        Arrays.fill(chars, ch);
        return new String(chars);
    }
    private static void certificates() throws Exception {
        X509Certificate ca = WifiConfigurationCertificates.ca(), client = WifiConfigurationCertificates.client();
        PrivateKey key = WifiConfigurationCertificates.key();
        check(ca.getBasicConstraints() >= 0 && client.getBasicConstraints() == -1, "Real DER certificate roles");
        client.verify(ca.getPublicKey());
        check(key.getEncoded().length > 0, "Real PKCS8 private key");
        WifiEnterpriseConfig value = new WifiEnterpriseConfig();
        value.setEapMethod(WifiEnterpriseConfig.Eap.TLS);
        value.setDomainSuffixMatch("example.test");
        check(!value.isServerCertValidationEnabled(), "Name constraint alone does not enable validation");
        value.setCaCertificate(ca);
        check(value.hasCaCertificate() && value.getCaCertificate() == ca && value.isServerCertValidationEnabled(), "Configured real CA validation");
        X509Certificate[] caArray = {ca, ca};
        value.setCaCertificates(caArray);
        caArray[0] = client;
        check(value.getCaCertificates() != caArray && value.getCaCertificate() == ca, "CA setter owns supplied array");
        WifiEnterpriseConfig copy = new WifiEnterpriseConfig(value);
        check(copy.getCaCertificates() != value.getCaCertificates() && Arrays.equals(copy.getCaCertificates(), value.getCaCertificates()), "Copy owns CA array");
        value.getCaCertificates()[0] = null;
        check(value.getCaCertificate() == null && copy.getCaCertificate() == ca, "CA getter exposes stored array, copy remains independent");
        expect(IllegalArgumentException.class, () -> value.setCaCertificate(client), "Reject end certificate as CA");
        check(value.getCaCertificates() == null, "Invalid CA clears stored certificate array");
        X509Certificate[] tooMany = new X509Certificate[101];
        Arrays.fill(tooMany, ca);
        expect(IllegalArgumentException.class, () -> value.setCaCertificates(tooMany), "CA count bound");
        value.setCaCertificates(new X509Certificate[]{ca});
        expect(NullPointerException.class, () -> value.setCaCertificates(new X509Certificate[]{null}), "Null CA entry");
        check(value.getCaCertificate() == ca, "Null CA entry fails before replacing prior array");
        value.setCaCertificates(new X509Certificate[0]);
        check(value.getCaCertificates() == null, "Empty CA array reads as absent");
        X509Certificate[] chain = {client, ca};
        value.setClientKeyEntryWithCertificateChain(key, chain);
        chain[0] = ca;
        check(value.getClientPrivateKey() == key && value.getClientCertificate() == client
                && value.getClientCertificateChain() != chain, "Client key and owned chain");
        copy = new WifiEnterpriseConfig(value);
        check(copy.getClientPrivateKey() == key && copy.getClientCertificateChain() != value.getClientCertificateChain(), "Copy owns chain array");
        expect(IllegalArgumentException.class, () -> value.setClientKeyEntryWithCertificateChain(null,
                new X509Certificate[]{client, ca}), "Certificate chain requires private key");
        expect(IllegalArgumentException.class, () -> value.setClientKeyEntry(key, ca), "First certificate must be end entity");
        expect(IllegalArgumentException.class, () -> value.setClientKeyEntryWithCertificateChain(key,
                new X509Certificate[]{client, client}), "Following certificate must be CA");
        expect(IllegalArgumentException.class, () -> value.setClientKeyEntryWithCertificateChain(key,
                new X509Certificate[]{client, ca, ca, ca, ca, ca}), "Client chain count bound");
        check(value.getClientPrivateKey() == key && value.getClientCertificate() == client, "Rejected client material preserves state");
        value.getClientCertificateChain()[0] = ca;
        check(value.getClientCertificate() == ca && copy.getClientCertificate() == client, "Client getter exposes stored array, copy remains independent");
        value.setClientKeyEntry(key, client);
        check(value.getClientCertificateChain().length == 1, "Single certificate setter wraps chain");
        value.setClientKeyEntryWithCertificateChain(key, new X509Certificate[0]);
        check(value.getClientPrivateKey() == key && value.getClientCertificateChain() == null, "Empty chain retains key without certificate");
        value.setClientKeyEntry(null, null);
        check(value.getClientPrivateKey() == null && value.getClientCertificate() == null, "Null client material clears values");
    }
    private static void vendorValues(boolean nativeParcel) throws Exception {
        ClassLoader loader = WifiConfiguration.class.getClassLoader();
        Class<?> type = Class.forName("android.net.wifi.OuiKeyedData", true, loader);
        Class<?> builder = Class.forName("android.net.wifi.OuiKeyedData$Builder", true, loader);
        PersistableBundle data = new PersistableBundle();
        data.putIntArray("channels", new int[]{1, 6, 11});
        PersistableBundle nested = new PersistableBundle();
        nested.putString("label", "configuration-value");
        data.putPersistableBundle("nested", nested);
        Object value = builder.getConstructor(int.class, PersistableBundle.class).newInstance(0x112233, data);
        value = builder.getMethod("build").invoke(value);
        PersistableBundle retained = (PersistableBundle) invoke(value, "getData", new Class<?>[0]);
        data.getIntArray("channels")[0] = 99;
        nested.putString("label", "changed");
        check(retained.getIntArray("channels")[0] == 1
                && "configuration-value".equals(retained.getPersistableBundle("nested").getString("label")), "Vendor owns deep bundle copy");
        Object equalBuilder = builder.getConstructor(int.class, PersistableBundle.class).newInstance(0x112233, retained);
        Object equal = builder.getMethod("build").invoke(equalBuilder);
        check(value.equals(equal) && value.hashCode() == equal.hashCode(), "Vendor content equality and hash");
        retained.putBoolean("changed", true);
        check(!value.equals(equal), "Vendor getter exposes Android mutable bundle ownership");
        for (int invalid : new int[]{0, -1, 0x1000000}) {
            expect(IllegalArgumentException.class, () -> builder.getMethod("build").invoke(
                    builder.getConstructor(int.class, PersistableBundle.class).newInstance(invalid, data)), "Invalid vendor OUI " + invalid);
        }
        expect(IllegalArgumentException.class, () -> builder.getMethod("build").invoke(
                builder.getConstructor(int.class, PersistableBundle.class).newInstance(0x112233, null)), "Null vendor data");
        WifiConfiguration configuration = new WifiConfiguration();
        List<Object> supplied = new ArrayList<>();
        supplied.add(value);
        invoke(configuration, "setVendorData", new Class<?>[]{List.class}, supplied);
        supplied.clear();
        List<?> stored = (List<?>) invoke(configuration, "getVendorData", new Class<?>[0]);
        WifiConfiguration copied = new WifiConfiguration(configuration);
        List<?> copiedData = (List<?>) invoke(copied, "getVendorData", new Class<?>[0]);
        check(stored.size() == 1 && copiedData.size() == 1 && copiedData != stored
                && copiedData.get(0) == value, "Vendor list setter and copy own lists, preserving element identity");
        if (nativeParcel) {
            @SuppressWarnings("unchecked") Parcelable.Creator<Parcelable> creator =
                    (Parcelable.Creator<Parcelable>) type.getField("CREATOR").get(null);
            check(value.equals(roundTrip((Parcelable) value, creator)), "Vendor native Parcel round trip");
            @SuppressWarnings("unchecked") Parcelable.Creator<WifiConfiguration> configCreator =
                    (Parcelable.Creator<WifiConfiguration>) WifiConfiguration.class.getField("CREATOR").get(null);
            WifiConfiguration restored = roundTrip(configuration, configCreator);
            List<?> restoredData = (List<?>) invoke(restored, "getVendorData", new Class<?>[0]);
            check(restoredData.size() == 1 && value.equals(restoredData.get(0)), "Configuration native Parcel retains nonempty typed vendor data");
        }
    }
    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0);
            T result = creator.createFromParcel(parcel);
            check(parcel.readInt() == SENTINEL && parcel.dataPosition() == parcel.dataSize(),
                    value.getClass().getSimpleName() + " Parcel consumes exactly its fields");
            return result;
        } finally { parcel.recycle(); }
    }
    private static void parcels() throws Exception {
        @SuppressWarnings("unchecked") Parcelable.Creator<WifiConfiguration> creator =
                (Parcelable.Creator<WifiConfiguration>) WifiConfiguration.class.getField("CREATOR").get(null);
        check(creator.newArray(3).length == 3 && WifiEnterpriseConfig.CREATOR.newArray(2).length == 2, "Creator array contracts");
        X509Certificate ca = WifiConfigurationCertificates.ca(), client = WifiConfigurationCertificates.client();
        PrivateKey key = WifiConfigurationCertificates.key();
        for (int security : new int[]{WifiConfiguration.SECURITY_TYPE_OPEN, WifiConfiguration.SECURITY_TYPE_PSK,
                WifiConfiguration.SECURITY_TYPE_EAP, WifiConfiguration.SECURITY_TYPE_SAE,
                WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT, WifiConfiguration.SECURITY_TYPE_DPP}) {
            WifiConfiguration value = new WifiConfiguration();
            value.SSID = "\"native-parcel-" + security + "\"";
            value.BSSID = "02:11:22:33:44:55";
            value.networkId = 100 + security;
            value.status = WifiConfiguration.Status.DISABLED;
            value.FQDN = "example.test";
            value.providerFriendlyName = "Example";
            value.isHomeProviderNetwork = true;
            value.roamingConsortiumIds = new long[]{1, 0xffffffffffL};
            value.preSharedKey = "parcel-secret";
            value.wepKeys[2] = "0011223344";
            value.wepTxKeyIndex = 2;
            value.priority = 19;
            value.hiddenSSID = true;
            value.setSecurityParams(security);
            value.setMacRandomizationSetting(WifiConfiguration.RANDOMIZATION_NON_PERSISTENT);
            value.setHttpProxy(ProxyInfo.buildDirectProxy("proxy.example.test", 8181));
            WifiEnterpriseConfig enterprise = value.enterpriseConfig;
            enterprise.setEapMethod(WifiEnterpriseConfig.Eap.TTLS);
            enterprise.setPhase2Method(WifiEnterpriseConfig.Phase2.MSCHAPV2);
            enterprise.setIdentity("identity@example.test");
            enterprise.setAnonymousIdentity("anonymous@example.test");
            enterprise.setPassword("enterprise-parcel-secret");
            enterprise.setDomainSuffixMatch("example.test");
            enterprise.setAltSubjectMatch("DNS:server.example.test");
            enterprise.setSubjectMatch("server");
            enterprise.setRealm("example.test");
            enterprise.setPlmn("00101");
            enterprise.setDecoratedIdentityPrefix("realm!");
            enterprise.setClientKeyPairAlias("test-alias");
            enterprise.setCaCertificate(ca);
            enterprise.setClientKeyEntryWithCertificateChain(key, new X509Certificate[]{client, ca});
            enterprise.setMinimumTlsVersion(WifiEnterpriseConfig.TLS_V1_3);
            enterprise.enableTrustOnFirstUse(true);
            invoke(value, "setPasspointUniqueId", new Class<?>[]{String.class}, "parcel-provider-" + security);
            if (security == WifiConfiguration.SECURITY_TYPE_DPP) {
                invoke(value, "setDppConfigurator", new Class<?>[]{byte[].class}, key.getEncoded());
            }
            WifiConfiguration actual = roundTrip(value, creator);
            check(actual.networkId == value.networkId && actual.status == value.status && actual.hiddenSSID
                    && actual.priority == 19 && actual.isHomeProviderNetwork && actual.wepTxKeyIndex == 2,
                    "Configuration native Parcel scalars " + security);
            check(value.SSID.equals(actual.SSID) && value.BSSID.equals(actual.BSSID)
                    && value.FQDN.equals(actual.FQDN) && value.providerFriendlyName.equals(actual.providerFriendlyName)
                    && value.preSharedKey.equals(actual.preSharedKey) && Arrays.equals(value.wepKeys, actual.wepKeys)
                    && Arrays.equals(value.roamingConsortiumIds, actual.roamingConsortiumIds), "Native Parcel identity and arrays " + security);
            for (int i = 0; i < masks(value).length; i++) {
                check(masks(value)[i].equals(masks(actual)[i]), "Configuration native Parcel security " + security + "/" + i);
            }
            check(actual.getKey().equals(value.getKey()) && actual.getHttpProxy().equals(value.getHttpProxy())
                    && actual.getMacRandomizationSetting() == value.getMacRandomizationSetting()
                    && actual.getRandomizedMacAddress().equals(value.getRandomizedMacAddress())
                    && actual.isPasspoint() && actual.isDppConfigurator() == value.isDppConfigurator(),
                    "Configuration native Parcel nested values " + security);
            assertEnterpriseParcel(enterprise, actual.enterpriseConfig);
            assertEnterpriseParcel(enterprise, roundTrip(enterprise, WifiEnterpriseConfig.CREATOR));
        }
        // Independent of CREATOR: distinguishes the original abbreviated writer.
        WifiConfiguration prefix = new WifiConfiguration();
        prefix.networkId = 71;
        prefix.status = WifiConfiguration.Status.DISABLED;
        Parcel parcel = Parcel.obtain();
        try {
            prefix.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            check(parcel.readInt() == 71 && parcel.readInt() == WifiConfiguration.Status.DISABLED,
                    "Pinned Parcel starts with networkId and status");
        } finally { parcel.recycle(); }
        // The pinned Android writer requires these containers, even though their
        // individual entries may be null. Verify each invalid state independently.
        WifiConfiguration missingWep = new WifiConfiguration();
        missingWep.wepKeys = null;
        WifiConfiguration missingProtocols = new WifiConfiguration();
        missingProtocols.allowedProtocols = null;
        Parcel invalid = Parcel.obtain();
        try {
            expect(NullPointerException.class, () -> missingWep.writeToParcel(invalid, 0),
                    "Native Parcel rejects a null WEP container");
            invalid.setDataSize(0);
            invalid.setDataPosition(0);
            expect(NullPointerException.class, () -> missingProtocols.writeToParcel(invalid, 0),
                    "Native Parcel rejects a null protocol container");
        } finally { invalid.recycle(); }
    }
    private static void assertEnterpriseParcel(WifiEnterpriseConfig expected, WifiEnterpriseConfig actual) throws Exception {
        check(expected.getEapMethod() == actual.getEapMethod() && expected.getPhase2Method() == actual.getPhase2Method()
                && expected.getMinimumTlsVersion() == actual.getMinimumTlsVersion()
                && expected.isTrustOnFirstUseEnabled() == actual.isTrustOnFirstUseEnabled(), "Enterprise Parcel policy values");
        check(expected.getIdentity().equals(actual.getIdentity()) && expected.getAnonymousIdentity().equals(actual.getAnonymousIdentity())
                && expected.getPassword().equals(actual.getPassword()) && expected.getRealm().equals(actual.getRealm())
                && expected.getPlmn().equals(actual.getPlmn()) && expected.getSubjectMatch().equals(actual.getSubjectMatch())
                && expected.getAltSubjectMatch().equals(actual.getAltSubjectMatch())
                && expected.getDomainSuffixMatch().equals(actual.getDomainSuffixMatch())
                && expected.getDecoratedIdentityPrefix().equals(actual.getDecoratedIdentityPrefix())
                && expected.getClientKeyPairAlias().equals(actual.getClientKeyPairAlias()), "Enterprise Parcel strings");
        check(Arrays.equals(expected.getCaCertificate().getEncoded(), actual.getCaCertificate().getEncoded())
                && Arrays.equals(expected.getClientPrivateKey().getEncoded(), actual.getClientPrivateKey().getEncoded())
                && Arrays.equals(expected.getClientCertificate().getEncoded(), actual.getClientCertificate().getEncoded())
                && actual.getClientCertificateChain().length == 2 && actual.isServerCertValidationEnabled(),
                "Enterprise Parcel reconstructs real certificate and private key bytes");
    }
    public static void run(boolean nativeParcel) throws Exception {
        checks = 0;
        defaultsAndCopy();
        constants();
        security();
        proxies();
        enterpriseFields();
        certificates();
        vendorValues(nativeParcel);
        if (nativeParcel) parcels();
        System.out.println("WifiConfigurationContractTest all passed: " + checks + " checks; parcel=" + nativeParcel);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        run(true);
    }
}
