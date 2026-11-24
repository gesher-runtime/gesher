/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.Context;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;

import com.android.internal.telephony.ISub;
import com.android.internal.telephony.ITelephony;

import java.io.PrintStream;
import java.util.List;

/**
 * The launcher's {@code mobile-data} command: Android's mobile data and data roaming
 * switches, which Gesher has no Settings APK to show. They are Windows' own connection
 * behavior for the modem (the SIM's default Mobile Broadband profile, docs/telephony.md),
 * so changing them changes Windows' cellular connection for the whole machine. The
 * command calls the {@code phone} service's own setters, as Settings' mobile network
 * screen does, as the shell (MODIFY_PHONE_STATE), and reports what Windows then shows.
 * Only with a cellular modem and an active SIM subscription.
 */
public final class MobileDataCommands {
    private MobileDataCommands() {}

    public static final String USAGE = "mobile-data [on|off] | mobile-data roaming on|off";

    public static int run(List<String> args) throws Exception {
        return run(args, ITelephony.Stub.asInterface(ServiceManager.getService(Context.TELEPHONY_SERVICE)),
                ISub.Stub.asInterface(ServiceManager.getService("isub")), System.out);
    }

    static int run(List<String> args, ITelephony phone, ISub subscriptions, PrintStream out)
            throws RemoteException {
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
        if (phone == null || subscriptions == null) throw new IllegalStateException("Telephony is unavailable");
        int subId = subscriptions.getDefaultDataSubId();
        boolean user;
        try {
            user = phone.isDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER);
        } catch (IllegalStateException noModem) {
            // The phone service's answer without a modem (TelephonyManager's null service).
            out.println("No cellular modem: there are no mobile data settings on this device");
            return 1;
        }
        if (!SubscriptionManager.isValidSubscriptionId(subId)) {
            out.println("No active SIM subscription: the mobile data settings belong to one");
            return 1;
        }
        boolean applied = true;
        if (value != null) {
            if (roaming) {
                phone.setDataRoamingEnabled(subId, value);
                applied = phone.isDataRoamingEnabled(subId) == value;
            } else {
                phone.setDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER, value,
                        "com.android.shell");
                applied = phone.isDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER) == value;
            }
            user = phone.isDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER);
        }
        if (!applied) out.println("Windows did not accept the change; nothing was changed");
        out.println("Mobile data: " + (user ? "on" : "off"));
        out.println("Data roaming: " + (phone.isDataRoamingEnabled(subId) ? "on" : "off"));
        out.println("These are Windows' cellular connection settings for this SIM (the whole machine):"
                + " off disconnects Windows' cellular data; roaming off keeps it off while roaming.");
        return applied ? 0 : 1;
    }
}
