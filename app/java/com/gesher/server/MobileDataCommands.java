/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.Context;
import android.os.ServiceManager;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;

import com.android.internal.telephony.ISub;
import com.android.internal.telephony.ITelephony;

import java.util.List;

/**
 * The launcher's {@code mobile-data} command: Android's mobile data and data roaming
 * switches (Settings.Global.MOBILE_DATA and DATA_ROAMING), which Gesher has no Settings APK
 * to show. It calls the {@code phone} service's own setters, as Settings' mobile network
 * screen does, as the shell (MODIFY_PHONE_STATE). Only with a cellular modem and an active
 * SIM subscription; the switches are Android's and do not change Windows' cellular
 * connection (docs/telephony.md).
 */
public final class MobileDataCommands {
    private MobileDataCommands() {}

    public static final String USAGE = "mobile-data [on|off] | mobile-data roaming on|off";

    public static int run(List<String> args) throws Exception {
        boolean roaming = !args.isEmpty() && args.get(0).equals("roaming");
        List<String> rest = roaming ? args.subList(1, args.size()) : args;
        Boolean value = null;
        if (rest.size() == 1) {
            if (rest.get(0).equals("on")) value = true;
            else if (rest.get(0).equals("off")) value = false;
        }
        if (rest.size() > 1 || rest.size() == 1 && value == null || roaming && value == null) {
            throw new IllegalArgumentException(USAGE);
        }
        ITelephony phone = ITelephony.Stub.asInterface(ServiceManager.getService(Context.TELEPHONY_SERVICE));
        ISub subscriptions = ISub.Stub.asInterface(ServiceManager.getService("isub"));
        if (phone == null || subscriptions == null) throw new IllegalStateException("Telephony is unavailable");
        int subId = subscriptions.getDefaultDataSubId();
        boolean user;
        try {
            user = phone.isDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER);
        } catch (IllegalStateException noModem) {
            // The phone service's answer without a modem (TelephonyManager's null service).
            System.out.println("No cellular modem: Android has no mobile data settings on this device");
            return 1;
        }
        if (!SubscriptionManager.isValidSubscriptionId(subId)) {
            System.out.println("No active SIM subscription: the mobile data settings belong to one");
            return 1;
        }
        if (value != null) {
            if (roaming) {
                phone.setDataRoamingEnabled(subId, value);
            } else {
                phone.setDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER, value,
                        "com.android.shell");
            }
            user = phone.isDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER);
        }
        System.out.println("Mobile data: " + (user ? "on" : "off"));
        System.out.println("Data roaming: " + (phone.isDataRoamingEnabled(subId) ? "on" : "off"));
        System.out.println("Android's settings, as apps see them; Windows' cellular settings decide"
                + " whether Windows uses its cellular connection.");
        return 0;
    }
}
