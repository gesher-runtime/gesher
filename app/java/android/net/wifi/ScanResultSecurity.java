/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.net.wifi;

import android.annotation.NonNull;
import java.util.ArrayList;
import java.util.List;

/**
 * Android 16 ScanResultUtil classification used by ScanResult.getSecurityTypes().
 *
 * <p>The ordered predicates are the pinned upstream implementation. The only
 * representation change is returning WifiInfo IDs directly instead of constructing
 * WifiConfiguration/SecurityParams objects and converting their IDs. It describes
 * the supplied capabilities; it does not assert support in the local Wi-Fi controller.
 */
final class ScanResultSecurity {
    private ScanResultSecurity() { }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a PSK network or not.
     * This checks if the provided capabilities string contains PSK encryption type or not.
     */
    private static boolean isScanResultForPskNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("PSK");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a WAPI-PSK network or not.
     * This checks if the provided capabilities string contains PSK encryption type or not.
     */
    private static boolean isScanResultForWapiPskNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("WAPI-PSK");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a WAPI-CERT
     * network or not.
     * This checks if the provided capabilities string contains PSK encryption type or not.
     */
    private static boolean isScanResultForWapiCertNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("WAPI-CERT");
    }

    private static boolean isScanResultForPmfMandatoryNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("[MFPR]");
    }

    private static boolean isScanResultForPmfCapableNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("[MFPC]");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a Passpoint R1/R2 network
     * or not. Passpoint R1/R2 requirements:
     * - Enterprise network not suite B.
     * - Interworking bit is set.
     * - HotSpot Release presents.
     */
    private static boolean isEapScanResultForPasspointR1R2Network(@NonNull ScanResult scanResult) {
        return scanResult.isPasspointNetwork();
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a Passpoint R3 network or
     * not. Passpoint R3 requirements:
     * - Enterprise network not suite B.
     * - Interworking bit is set.
     * - HotSpot Release presents.
     * - PMF is mandatory.
     */
    private static boolean isEapScanResultForPasspointR3Network(@NonNull ScanResult scanResult) {
        if (!isScanResultForPmfMandatoryNetwork(scanResult)) return false;

        return scanResult.isPasspointNetwork();
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to
     * a WPA3 Enterprise transition network or not.
     *
     * See Section 3.3 WPA3-Enterprise transition mode in WPA3 Specification
     * - Enable at least EAP/SHA1 and EAP/SHA256 AKM suites.
     * - Not enable WPA1 version 1, WEP, and TKIP.
     * - Management Frame Protection Capable is set.
     * - Management Frame Protection Required is not set.
     */
    private static boolean isScanResultForWpa3EnterpriseTransitionNetwork(
            @NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("EAP/SHA1")
                && scanResult.capabilities.contains("EAP/SHA256")
                && scanResult.capabilities.contains("RSN")
                && !scanResult.capabilities.contains("WEP")
                && !scanResult.capabilities.contains("TKIP")
                && !isScanResultForPmfMandatoryNetwork(scanResult)
                && isScanResultForPmfCapableNetwork(scanResult);
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to
     * a WPA3 Enterprise only network or not.
     *
     * See Section 3.2 WPA3-Enterprise only mode in WPA3 Specification
     * - Enable at least EAP/SHA256 AKM suite.
     * - Not enable EAP/SHA1 AKM suite.
     * - Not enable WPA1 version 1, WEP, and TKIP.
     * - Management Frame Protection Capable is set.
     * - Management Frame Protection Required is set.
     */
    private static boolean isScanResultForWpa3EnterpriseOnlyNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("EAP/SHA256")
                && !scanResult.capabilities.contains("EAP/SHA1")
                && scanResult.capabilities.contains("RSN")
                && !scanResult.capabilities.contains("WEP")
                && !scanResult.capabilities.contains("TKIP")
                && isScanResultForPmfMandatoryNetwork(scanResult)
                && isScanResultForPmfCapableNetwork(scanResult);
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a WPA3-Enterprise 192-bit
     * mode network or not.
     * This checks if the provided capabilities comply these conditions:
     * - Enable SUITE-B-192 AKM.
     * - Not enable EAP/SHA1 AKM suite.
     * - Not enable WPA1 version 1, WEP, and TKIP.
     * - Management Frame Protection Required is set.
     */
    private static boolean isScanResultForEapSuiteBNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("SUITE_B_192")
                && scanResult.capabilities.contains("RSN")
                && !scanResult.capabilities.contains("WEP")
                && !scanResult.capabilities.contains("TKIP")
                && isScanResultForPmfMandatoryNetwork(scanResult);
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a WEP network or not.
     * This checks if the provided capabilities string contains WEP encryption type or not.
     */
    private static boolean isScanResultForWepNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("WEP");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to OWE network.
     * This checks if the provided capabilities string contains OWE or not.
     */
    private static boolean isScanResultForOweNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("OWE");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to OWE transition network.
     * This checks if the provided capabilities string contains OWE_TRANSITION or not.
     */
    private static boolean isScanResultForOweTransitionNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("OWE_TRANSITION");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to SAE network.
     * This checks if the provided capabilities string contains SAE or not.
     */
    private static boolean isScanResultForSaeNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("SAE");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to PSK-SAE transition
     * network. This checks if the provided capabilities string contains both PSK and SAE or not.
     */
    private static boolean isScanResultForPskSaeTransitionNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("PSK") && scanResult.capabilities.contains("SAE");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to FILS SHA256 network.
     * This checks if the provided capabilities string contains FILS-SHA256 or not.
     */
    private static boolean isScanResultForFilsSha256Network(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("FILS-SHA256");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to FILS SHA384 network.
     * This checks if the provided capabilities string contains FILS-SHA384 or not.
     */
    private static boolean isScanResultForFilsSha384Network(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("FILS-SHA384");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to DPP network.
     * This checks if the provided capabilities string contains DPP or not.
     */
    private static boolean isScanResultForDppNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("DPP");
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to only WPA-Personal network.
     * This checks if the provided capabilities string contains WPA and not RSN.
     */
    private static boolean isScanResultForWpaPersonalOnlyNetwork(@NonNull ScanResult scanResult) {
        return isScanResultForPskNetwork(scanResult) && !scanResult.capabilities.contains("RSN");
    }

    /**
     *  Helper method to check if the provided |scanResult| corresponds to an unknown amk network.
     *  This checks if the provided capabilities string contains ? or not.
     */
    private static boolean isScanResultForUnknownAkmNetwork(@NonNull ScanResult scanResult) {
        return scanResult.capabilities.contains("?");
    }

    /**
     *  Helper method to check if the provided |scanResult| corresponds to a pure PSK network.
     */
    private static boolean isScanResultForPskOnlyNetwork(@NonNull ScanResult r) {
        return ScanResultSecurity.isScanResultForPskNetwork(r)
                && !ScanResultSecurity.isScanResultForSaeNetwork(r);
    }

    /**
     *  Helper method to check if the provided |scanResult| corresponds to a pure SAE network.
     */
    private static boolean isScanResultForSaeOnlyNetwork(@NonNull ScanResult r) {
        return !ScanResultSecurity.isScanResultForPskNetwork(r)
                && ScanResultSecurity.isScanResultForSaeNetwork(r);
    }

    /**
     *  Helper method to check if the provided |scanResult| corresponds to a pure OPEN network.
     */
    private static boolean isScanResultForOpenOnlyNetwork(@NonNull ScanResult r) {
        return ScanResultSecurity.isScanResultForOpenNetwork(r)
                && !ScanResultSecurity.isScanResultForOweNetwork(r);
    }

    /**
     *  Helper method to check if the provided |scanResult| corresponds to a pure OWE network.
     */
    private static boolean isScanResultForOweOnlyNetwork(@NonNull ScanResult r) {
        return !ScanResultSecurity.isScanResultForOweTransitionNetwork(r)
                && ScanResultSecurity.isScanResultForOweNetwork(r);
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to a pure WPA2 Enterprise
     * network.
     */
    private static boolean isScanResultForWpa2EnterpriseOnlyNetwork(@NonNull ScanResult scanResult) {
        return (scanResult.capabilities.contains("EAP/SHA1")
                        || scanResult.capabilities.contains("EAP/SHA256")
                        || scanResult.capabilities.contains("FT/EAP")
                        || scanResult.capabilities.contains("EAP-FILS"))
                && !isScanResultForWpa3EnterpriseOnlyNetwork(scanResult)
                && !isScanResultForWpa3EnterpriseTransitionNetwork(scanResult);
    }

    /**
     * Helper method to check if the provided |scanResult| corresponds to an open network or not.
     * This checks if the provided capabilities string does not contain either of WEP, PSK, SAE
     * EAP, or unknown encryption types or not.
     */
    private static boolean isScanResultForOpenNetwork(@NonNull ScanResult scanResult) {
        return (!(isScanResultForWepNetwork(scanResult)
                || isScanResultForPskNetwork(scanResult)
                || isScanResultForWpa2EnterpriseOnlyNetwork(scanResult)
                || isScanResultForSaeNetwork(scanResult)
                || isScanResultForWpa3EnterpriseTransitionNetwork(scanResult)
                || isScanResultForWpa3EnterpriseOnlyNetwork(scanResult)
                || isScanResultForWapiPskNetwork(scanResult)
                || isScanResultForWapiCertNetwork(scanResult)
                || isScanResultForEapSuiteBNetwork(scanResult)
                || isScanResultForDppNetwork(scanResult)
                || isScanResultForUnknownAkmNetwork(scanResult)));
    }

    static int[] getSecurityTypes(
            @NonNull ScanResult scanResult) {
        List<Integer> list = new ArrayList<>();

        // Open network & its upgradable types
        if (ScanResultSecurity.isScanResultForOweTransitionNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_OPEN);
            list.add(WifiInfo.SECURITY_TYPE_OWE);
            return toArray(list);
        } else if (ScanResultSecurity.isScanResultForOweNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_OWE);
            return toArray(list);
        } else if (ScanResultSecurity.isScanResultForOpenNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_OPEN);
            return toArray(list);
        }

        // WEP network which has no upgradable type
        if (ScanResultSecurity.isScanResultForWepNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_WEP);
            return toArray(list);
        }

        // WAPI PSK network which has no upgradable type
        if (ScanResultSecurity.isScanResultForWapiPskNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_WAPI_PSK);
            return toArray(list);
        }

        // WAPI CERT network which has no upgradable type
        if (ScanResultSecurity.isScanResultForWapiCertNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_WAPI_CERT);
            return toArray(list);
        }

        // WPA2 personal network & its upgradable types
        if (ScanResultSecurity.isScanResultForPskNetwork(scanResult)
                && ScanResultSecurity.isScanResultForSaeNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_PSK);
            list.add(WifiInfo.SECURITY_TYPE_SAE);
            return toArray(list);
        } else if (ScanResultSecurity.isScanResultForPskNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_PSK);
            return toArray(list);
        } else if (ScanResultSecurity.isScanResultForSaeNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_SAE);
            return toArray(list);
        } else if (ScanResultSecurity.isScanResultForDppNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_DPP);
            return toArray(list);
        }

        boolean isEapNetworkAndNotSuiteB = false;
        // WPA3 Enterprise 192-bit mode, WPA2/WPA3 enterprise network & its upgradable types
        if (ScanResultSecurity.isScanResultForEapSuiteBNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT);
        } else if (ScanResultSecurity.isScanResultForWpa3EnterpriseTransitionNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_EAP);
            list.add(WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE);
            isEapNetworkAndNotSuiteB = true;
        } else if (ScanResultSecurity.isScanResultForWpa3EnterpriseOnlyNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE);
            isEapNetworkAndNotSuiteB = true;
        } else if (ScanResultSecurity.isScanResultForWpa2EnterpriseOnlyNetwork(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_EAP);
            isEapNetworkAndNotSuiteB = true;
        }
        if (!isEapNetworkAndNotSuiteB) {
            return toArray(list);
        }
        // An Enterprise network might be a Passpoint network as well.
        // R3 network might be also a valid R1/R2 network.
        if (isEapScanResultForPasspointR1R2Network(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_PASSPOINT_R1_R2);
        }
        if (isEapScanResultForPasspointR3Network(scanResult)) {
            list.add(WifiInfo.SECURITY_TYPE_PASSPOINT_R3);
        }
        return toArray(list);
    }

    private static int[] toArray(List<Integer> types) {
        int[] result = new int[types.size()];
        for (int i = 0; i < result.length; i++) result[i] = types.get(i);
        return result;
    }
}
