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
import java.util.concurrent.atomic.AtomicReference;

/**
 * The launcher's {@code mobile-data} command: Android's mobile data and data roaming
 * switches, which Gesher has no Settings APK to show. They are Windows' own connection
 * behavior for the modem (the SIM's default Mobile Broadband profile, docs/telephony.md),
 * so changing them changes Windows' cellular connection for the whole machine. The
 * command calls the {@code phone} service's own setters, as Settings' mobile network
 * screen does, as the shell (MODIFY_PHONE_STATE). The setters return nothing (as on
 * AOSP), so the command then reads Windows itself and reports success only when Windows
 * shows the requested setting (and, for data off, its connection down). Only with a
 * cellular modem and an active SIM subscription.
 */
public final class MobileDataCommands {
    private MobileDataCommands() {}

    public static final String USAGE = "mobile-data [on|off] | mobile-data roaming on|off";

    public static int run(List<String> args) throws Exception {
        return run(args, ITelephony.Stub.asInterface(ServiceManager.getService(Context.TELEPHONY_SERVICE)),
                ISub.Stub.asInterface(ServiceManager.getService("isub")), WindowsCellular.WINDOWS, System.out);
    }

    static int run(List<String> args, ITelephony phone, ISub subscriptions, WindowsCellular.Source windows,
            PrintStream out) throws RemoteException {
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
        boolean ok = true;
        if (value != null) {
            if (roaming) {
                phone.setDataRoamingEnabled(subId, value);
            } else {
                phone.setDataEnabledForReason(subId, TelephonyManager.DATA_ENABLED_REASON_USER, value,
                        "com.android.shell");
            }
            ok = confirm(phone, subId, windows, roaming, value, user, out);
        }
        out.println("Mobile data: " + (phone.isDataEnabledForReason(subId,
                TelephonyManager.DATA_ENABLED_REASON_USER) ? "on" : "off"));
        out.println("Data roaming: " + (phone.isDataRoamingEnabled(subId) ? "on" : "off"));
        out.println("These are Windows' cellular connection settings for this SIM (the whole machine):"
                + " off disconnects Windows' cellular data; roaming off keeps it off while roaming.");
        return ok ? 0 : 1;
    }

    /**
     * Whether Windows shows the requested setting, read from Windows itself: data on is
     * auto or auto-home and off is manual with the connection down (for a change from on;
     * a connection made by hand while data was already off is left alone, as an unchanged
     * switch changes nothing); roaming is auto or auto-home while data is on, and while it
     * is off (manual, where Windows keeps no roaming choice) the phone service's answer.
     */
    private static boolean confirm(ITelephony phone, int subId, WindowsCellular.Source windows,
            boolean roaming, boolean value, boolean wasOn, PrintStream out) throws RemoteException {
        WindowsCellular.Modem modem = read(windows);
        if (modem == null || !modem.knownMode()) {
            out.println("Windows' cellular settings could not be read, so the change is not confirmed"
                    + " (Windows may or may not have taken it)");
            return false;
        }
        String mode = modem.connectionMode;
        boolean taken;
        if (roaming) {
            taken = WindowsCellular.MODE_MANUAL.equals(mode) ? phone.isDataRoamingEnabled(subId) == value
                    : mode.equals(value ? WindowsCellular.MODE_AUTO : WindowsCellular.MODE_AUTO_HOME);
        } else {
            taken = WindowsCellular.MODE_MANUAL.equals(mode) != value;
        }
        if (!taken) {
            out.println("Windows did not take the change, or did not answer in time (a late change"
                    + " may still apply); Windows' setting is as shown below");
            return false;
        }
        if (!roaming && !value && wasOn && !disconnected(windows, modem)) {
            out.println("Windows stored mobile data off but its cellular connection is still up:"
                    + " Windows refused to disconnect it");
            return false;
        }
        return true;
    }

    /**
     * Waits up to ten seconds for Windows' connection to go down: the disconnect completes
     * asynchronously in WwanSvc. Each step reads Windows afresh.
     */
    private static boolean disconnected(WindowsCellular.Source windows, WindowsCellular.Modem modem) {
        for (int i = 0; ; i++) {
            if (modem != null && !up(modem)) return true;
            if (i >= sWaitSteps) return false;
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
            modem = read(windows);
        }
    }

    private static boolean up(WindowsCellular.Modem modem) {
        return modem.activation == WindowsCellular.ACTIVATION_ACTIVATED
                || modem.activation == WindowsCellular.ACTIVATION_ACTIVATING
                || modem.activation == WindowsCellular.ACTIVATION_DEACTIVATING;
    }

    /** One bounded read of Windows' modem (phone 0, the first interface), or null. */
    private static WindowsCellular.Modem read(WindowsCellular.Source windows) {
        AtomicReference<String> raw = new AtomicReference<>();
        Thread reader = new Thread(() -> raw.set(windows.snapshot()), "mobile-data-read");
        reader.setDaemon(true);
        reader.start();
        try {
            reader.join(sReadTimeoutMillis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (reader.isAlive()) return null;
        WindowsCellular.Snapshot snapshot = WindowsCellular.parse(raw.get());
        return snapshot == null || !"ok".equals(snapshot.status) ? null : snapshot.primary();
    }

    /** One-second steps of the disconnect wait, and the bound of each read; tests shorten them. */
    static int sWaitSteps = 10;
    static long sReadTimeoutMillis = 5000;
}
