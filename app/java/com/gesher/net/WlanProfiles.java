/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.wifi.WifiConfiguration;
import java.nio.charset.StandardCharsets;

/**
 * Windows WLAN profile XML for a user-approved Android network (WLAN_profile schema v1).
 * Only security Windows can express faithfully is produced: open, OWE, WPA2-Personal
 * and WPA3-Personal (SAE). Anything else (WEP, enterprise, WAPI) returns null and the
 * caller reports the request as unfulfillable instead of joining a different network.
 * The SSID is written as hex, so its original bytes reach Windows unchanged.
 */
public final class WlanProfiles {
    private WlanProfiles() { }

    /** Gesher's profile names; nothing else in a Windows profile name is app-controlled. */
    public static final String PREFIX = "Gesher-";

    public static String xml(String name, byte[] ssid, boolean hidden, int securityType,
            String preSharedKey, boolean autoConnect) {
        if (name == null || !name.startsWith(PREFIX) || name.length() > 200 || ssid == null
                || ssid.length == 0 || ssid.length > 32) return null;
        String authentication, encryption;
        switch (securityType) {
            case WifiConfiguration.SECURITY_TYPE_OPEN: authentication = "open"; encryption = "none"; break;
            case WifiConfiguration.SECURITY_TYPE_OWE: authentication = "OWE"; encryption = "AES"; break;
            case WifiConfiguration.SECURITY_TYPE_PSK: authentication = "WPA2PSK"; encryption = "AES"; break;
            case WifiConfiguration.SECURITY_TYPE_SAE: authentication = "WPA3SAE"; encryption = "AES"; break;
            default: return null;
        }
        String key = null, keyType = null;
        if (securityType == WifiConfiguration.SECURITY_TYPE_PSK || securityType == WifiConfiguration.SECURITY_TYPE_SAE) {
            if (preSharedKey == null) return null;
            if (preSharedKey.length() >= 2 && preSharedKey.startsWith("\"") && preSharedKey.endsWith("\"")) {
                key = preSharedKey.substring(1, preSharedKey.length() - 1);
                keyType = "passPhrase";
                // WPA2 passphrases are 8..63 printable ASCII; SAE passwords may be longer.
                if (key.isEmpty() || key.length() > 128 || !printableAscii(key)
                        || securityType == WifiConfiguration.SECURITY_TYPE_PSK && (key.length() < 8 || key.length() > 63)) {
                    return null;
                }
            } else if (securityType == WifiConfiguration.SECURITY_TYPE_PSK && preSharedKey.matches("[0-9a-fA-F]{64}")) {
                key = preSharedKey;
                keyType = "networkKey";
            } else {
                return null;
            }
        } else if (preSharedKey != null && !preSharedKey.isEmpty() && !"*".equals(preSharedKey)) {
            return null;
        }
        StringBuilder out = new StringBuilder(512);
        out.append("<?xml version=\"1.0\"?>\n<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">")
                .append("<name>").append(escape(name)).append("</name>")
                .append("<SSIDConfig><SSID><hex>").append(hex(ssid)).append("</hex></SSID>")
                .append("<nonBroadcast>").append(hidden).append("</nonBroadcast></SSIDConfig>")
                .append("<connectionType>ESS</connectionType>")
                .append("<connectionMode>").append(autoConnect ? "auto" : "manual").append("</connectionMode>")
                .append("<MSM><security><authEncryption><authentication>").append(authentication)
                .append("</authentication><encryption>").append(encryption)
                .append("</encryption><useOneX>false</useOneX></authEncryption>");
        if (key != null) {
            out.append("<sharedKey><keyType>").append(keyType).append("</keyType><protected>false</protected><keyMaterial>")
                    .append(escape(key)).append("</keyMaterial></sharedKey>");
        }
        out.append("</security></MSM></WLANProfile>");
        return out.toString();
    }

    /** The security type Windows can express for this configuration, or -1. */
    public static int securityType(WifiConfiguration config) {
        if (config == null) return -1;
        int type;
        try {
            // The first (default) parameters are the network's own; later entries are
            // auto-upgrade alternatives that the AP may not offer.
            type = config.getDefaultSecurityParams().getSecurityType();
        } catch (RuntimeException unusable) {
            return -1;
        }
        switch (type) {
            case WifiConfiguration.SECURITY_TYPE_SAE: case WifiConfiguration.SECURITY_TYPE_PSK:
            case WifiConfiguration.SECURITY_TYPE_OWE: case WifiConfiguration.SECURITY_TYPE_OPEN:
                return type;
            default:
                return -1;
        }
    }

    private static boolean printableAscii(String value) {
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c < 0x20 || c > 0x7e) return false;
        }
        return true;
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format("%02X", b & 255));
        return out.toString();
    }

    static String escape(String value) {
        StringBuilder out = new StringBuilder(value.length() + 16);
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '&': out.append("&amp;"); break;
                case '<': out.append("&lt;"); break;
                case '>': out.append("&gt;"); break;
                case '"': out.append("&quot;"); break;
                case '\'': out.append("&apos;"); break;
                default: out.append(c);
            }
        }
        return out.toString();
    }

    /**
     * The SSID bytes of a WifiConfiguration.SSID: quoted UTF-8 text, or the unquoted hex
     * form Android uses for SSIDs that are not UTF-8 (WifiSsid). Null when malformed or
     * outside 1..32 bytes.
     */
    public static byte[] ssidBytes(String ssid) {
        if (ssid == null) return null;
        byte[] bytes;
        try {
            bytes = android.net.wifi.WifiSsid.fromString(ssid).getBytes();
        } catch (RuntimeException malformed) {
            return null;
        }
        return bytes.length < 1 || bytes.length > 32 ? null : bytes;
    }
}
