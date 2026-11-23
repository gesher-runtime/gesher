/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.util.ArrayMap;
import com.android.internal.telephony.IOnSubscriptionsChangedListener;
import com.android.internal.telephony.IPhoneStateListener;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Production telephony services over an explicit test boundary: synthetic modem
 * snapshots (WindowsCellular.Source) and caller permissions (TelephonyService.Callers).
 * Synthetic modems exist only here; the services, rules, parser and translations are the
 * production classes. No ART, Binder transaction, Windows API or radio is exercised.
 */
public final class TelephonyHost {
    private static int checks;

    static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
        checks++;
    }

    static void equal(Object actual, Object expected, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected " + expected + ", got " + actual);
        }
        checks++;
    }

    interface Call { Object run() throws Exception; }

    static void denied(Call call, String what) {
        try {
            call.run();
        } catch (SecurityException expected) {
            checks++;
            return;
        } catch (Exception e) {
            throw new AssertionError(what + ": " + e, e);
        }
        throw new AssertionError(what + ": no SecurityException");
    }

    static <T extends Throwable> void throwsType(Class<T> type, Call call, String what) {
        try {
            call.run();
        } catch (Throwable t) {
            if (type.isInstance(t)) { checks++; return; }
            throw new AssertionError(what + ": " + t, t);
        }
        throw new AssertionError(what + ": nothing thrown");
    }

    // ---- The caller boundary. ----

    static final class App {
        final String name; final int uid; final int target;
        final Set<String> permissions = new HashSet<>();
        final Map<String, Integer> ops = new HashMap<>();
        App(String name, int uid, int target, String... permissions) {
            this.name = name; this.uid = uid; this.target = target;
            this.permissions.addAll(List.of(permissions));
        }
        App op(String op, int mode) { ops.put(op, mode); return this; }
    }

    /**
     * AOSP's AppOpsManager.opToDefaultMode for the operations the rules note (the
     * manager's static initializer needs a native Binder this host JVM lacks).
     */
    static final Map<String, Integer> DEFAULT_MODES = Map.of(
            AppOpsManager.OPSTR_READ_PHONE_STATE, AppOpsManager.MODE_ALLOWED,
            AppOpsManager.OPSTR_USE_ICC_AUTH_WITH_DEVICE_IDENTIFIER, AppOpsManager.MODE_DEFAULT,
            AppOpsManager.OPSTR_READ_DEVICE_IDENTIFIERS, AppOpsManager.MODE_ERRORED,
            AppOpsManager.OPSTR_WRITE_SMS, AppOpsManager.MODE_IGNORED,
            AppOpsManager.OPSTR_READ_PHONE_NUMBERS, AppOpsManager.MODE_ALLOWED,
            AppOpsManager.OPSTR_READ_SMS, AppOpsManager.MODE_ALLOWED,
            AppOpsManager.OPSTR_FINE_LOCATION, AppOpsManager.MODE_ALLOWED,
            AppOpsManager.OPSTR_COARSE_LOCATION, AppOpsManager.MODE_ALLOWED);

    static final class Fixture implements TelephonyService.Callers {
        App current;
        boolean location = true;
        @Override public TelephonyService.Caller authenticate(String claimed, String tag) {
            if (claimed != null && !claimed.equals(current.name)) {
                throw new SecurityException("Caller cannot act for another package");
            }
            return new TelephonyService.Caller(current.name, tag, current.uid, current.target, false);
        }
        @Override public boolean granted(TelephonyService.Caller c, String permission) {
            return current.permissions.contains(permission);
        }
        @Override public int noteOp(TelephonyService.Caller c, String op) {
            Integer mode = current.ops.get(op);
            if (mode != null) return mode;
            Integer initial = DEFAULT_MODES.get(op);
            if (initial == null) throw new AssertionError("Unexpected app op " + op);
            return initial;
        }
        @Override public boolean locationEnabled() { return location; }
    }

    // ---- Synthetic modem snapshots, in the native adapter's format. ----

    static String hex(String text) {
        StringBuilder out = new StringBuilder();
        for (byte b : text.getBytes(StandardCharsets.UTF_8)) out.append(String.format("%02x", b & 0xff));
        return out.toString();
    }

    static final class ModemSpec {
        String id = "{0D7F7A43-0000-4000-8000-00000000C311}";
        int ready = 1, cellular = 1, register = 3, dataClass = 0x20, signal = 20, error = 99, activation = 1;
        String imei = "356938035643809", imsi = "425010123456789", iccid = "8997201000000000001F";
        String number = "+972501234567", home = "42501", homeName = "Partner", regId = "42501",
                regName = "Partner IL";
        String line() {
            String[] f = {"modem", hex(id), "" + ready, "" + cellular, "1", "63", "3", hex(imei),
                hex("Fixture"), hex("WWAN"), hex("FW 1.0"),
                imsi == null ? "-" : hex(imsi), iccid == null ? "-" : hex(iccid),
                number == null ? "-" : hex(number), home == null ? "-" : hex(home),
                homeName == null ? "-" : hex(homeName), "" + register, hex(regId), hex(regName), "",
                "" + dataClass, "" + signal, "" + error, activation < 0 ? "-" : "" + activation};
            return String.join("\t", f);
        }
    }

    static final class Source implements WindowsCellular.Source {
        String text;
        @Override public String snapshot() { return text; }
        void set(ModemSpec... modems) {
            StringBuilder out = new StringBuilder("status\tok\t00000000\n");
            for (ModemSpec m : modems) out.append(m.line()).append('\n');
            text = out.toString();
        }
    }

    static final class Properties implements TelephonyService.PropertySink {
        final List<Integer> simStates = new ArrayList<>();
        @Override public void publish(WindowsCellular.Modem modem, int simState) { simStates.add(simState); }
    }

    /** A remote (proxy) binder whose process death the fixture can trigger. */
    static final class Remote {
        final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        final IBinder binder = (IBinder) Proxy.newProxyInstance(TelephonyHost.class.getClassLoader(),
                new Class<?>[] {IBinder.class}, (proxy, method, args) -> {
                    switch (method.getName()) {
                        case "hashCode": return System.identityHashCode(proxy);
                        case "equals": return proxy == args[0];
                        case "isBinderAlive": case "pingBinder": return true;
                        case "linkToDeath": recipients.add((IBinder.DeathRecipient) args[0]); return null;
                        case "unlinkToDeath": return recipients.remove(args[0]);
                        default: return method.getReturnType() == boolean.class ? false : null;
                    }
                });
        void die() {
            for (IBinder.DeathRecipient r : new ArrayList<>(recipients)) r.binderDied();
        }
    }

    static final class Listener extends IPhoneStateListener.Default {
        final Remote remote = new Remote();
        final IBinder binder = remote.binder;
        final List<String> events = new ArrayList<>();
        ServiceState state;
        SignalStrength signal;
        @Override public IBinder asBinder() { return binder; }
        @Override public void onServiceStateChanged(ServiceState s) { events.add("service"); state = s; }
        @Override public void onSignalStrengthChanged(int asu) { events.add("asu"); }
        @Override public void onSignalStrengthsChanged(SignalStrength s) { events.add("signal"); signal = s; }
        @Override public void onDataConnectionStateChanged(int state, int type) { events.add("data:" + state + ":" + type); }
        @Override public void onCallStateChanged(int state) { events.add("call:" + state); }
        android.telephony.TelephonyDisplayInfo display;
        @Override public void onDisplayInfoChanged(android.telephony.TelephonyDisplayInfo info) {
            events.add("display"); display = info;
        }
        @Override public void onActiveDataSubIdChanged(int subId) { events.add("activeData:" + subId); }
        @Override public void onDataActivity(int activity) { events.add("activity:" + activity); }
        @Override public void onUserMobileDataStateChanged(boolean on) { events.add("userData:" + on); }
        @Override public void onDataEnabledChanged(boolean on, int reason) {
            events.add("dataEnabled:" + on + ":" + reason);
        }
        @Override public void onCellInfoChanged(List<android.telephony.CellInfo> cells) {
            events.add("cellInfo:" + cells.size());
        }
        @Override public void onCellLocationChanged(android.telephony.CellIdentity identity) {
            events.add("cellLocation:" + identity);
        }
    }

    static final class CellInfoCallback extends android.telephony.ICellInfoCallback.Default {
        final List<String> results = new ArrayList<>();
        @Override public void onCellInfo(List<android.telephony.CellInfo> state) { results.add("cells:" + state.size()); }
        @Override public void onError(int code, String exception, String message) {
            results.add("error:" + code + ":" + exception);
        }
    }

    static final class IntResult extends com.android.internal.telephony.IIntegerConsumer.Default {
        final List<Integer> results = new ArrayList<>();
        @Override public void accept(int result) { results.add(result); }
    }

    static final class GbaCallback extends android.telephony.IBootstrapAuthenticationCallback.Default {
        final List<Integer> reasons = new ArrayList<>();
        @Override public void onAuthenticationFailure(int token, int reason) { reasons.add(reason); }
    }

    static final class Receiver extends android.os.ResultReceiver {
        final List<Integer> codes = new ArrayList<>();
        final List<android.os.Bundle> data = new ArrayList<>();
        Receiver() { super(null); }
        @Override protected void onReceiveResult(int code, android.os.Bundle bundle) {
            codes.add(code); data.add(bundle);
        }
    }

    static final class SubscriptionsListener extends IOnSubscriptionsChangedListener.Default {
        final Remote remote = new Remote();
        final IBinder binder = remote.binder;
        int calls;
        @Override public IBinder asBinder() { return binder; }
        @Override public void onSubscriptionsChanged() { calls++; }
    }

    static Set<String> features(WindowsCellular.Snapshot boot) {
        ArrayMap<String, FeatureInfo> features = new ArrayMap<>();
        TelephonyService.addTelephonyFeatures(features, boot);
        return new HashSet<>(features.keySet());
    }

    // ---- Scenarios. ----

    static final App PLAIN = new App("com.example.plain", 10100, 34);
    static final App STATE = new App("com.example.state", 10101, 34, TelephonyService.READ_PHONE_STATE);
    static final App LEGACY = new App("com.example.legacy", 10102, 28, TelephonyService.READ_PHONE_STATE);
    static final App NUMBERS = new App("com.example.numbers", 10103, 34,
            TelephonyService.READ_PHONE_STATE, TelephonyService.READ_PHONE_NUMBERS);
    static final App PRIVILEGED = new App("com.example.privileged", 10104, 34,
            TelephonyService.READ_PRIVILEGED_PHONE_STATE, TelephonyService.READ_PHONE_STATE);
    static final App BASIC = new App("com.example.basic", 10105, 34, TelephonyService.READ_BASIC_PHONE_STATE);
    // Android grants coarse location with fine.
    static final App LOCATED = new App("com.example.located", 10106, 34,
            TelephonyService.READ_PHONE_STATE, TelephonyService.FINE, TelephonyService.COARSE);
    static final App REVOKED = new App("com.example.revoked", 10107, 34, TelephonyService.READ_PHONE_STATE)
            .op(AppOpsManager.OPSTR_READ_PHONE_STATE, AppOpsManager.MODE_IGNORED);

    static void noModem() throws Exception {
        Source source = new Source();
        source.text = "status\tnone\t80040154\n";
        WindowsCellular.Snapshot parsed = WindowsCellular.parse(source.text);
        check(parsed != null && parsed.modems.isEmpty(), "No-WWAN snapshot parses");
        // The boot probe treats "none" as no modem.
        Properties properties = new Properties();
        TelephonyService.Cellular cellular = new TelephonyService.Cellular(source, null, properties);
        check(!cellular.present(), "No phone without a modem");
        check(features(null).isEmpty(), "No telephony features without a modem");
        check(features(parsed).isEmpty(), "No telephony features for an empty snapshot");
        Fixture callers = new Fixture();
        TelephonyService.Endpoints t = new TelephonyService.Endpoints(cellular, callers);
        check(!cellular.refresh(), "Nothing polls without a modem");
        check(properties.simStates.isEmpty(), "No telephony properties are written without a modem");

        callers.current = STATE;
        // ADR 0087: no SIM, not "still loading". getSimState() asks for
        // DEFAULT_SIM_SLOT_INDEX (Integer.MAX_VALUE) without a default subscription.
        equal(t.phone.getSimStateForSlotIndex(0), TelephonyManager.SIM_STATE_ABSENT, "No SIM in slot 0");
        equal(t.phone.getSimStateForSlotIndex(SubscriptionManager.DEFAULT_SIM_SLOT_INDEX),
                TelephonyManager.SIM_STATE_ABSENT, "getSimState()'s default slot");
        equal(t.phone.getSimStateForSlotIndex(-1), TelephonyManager.SIM_STATE_UNKNOWN, "Invalid slot");
        equal(t.phone.getDataNetworkTypeForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                STATE.name, null), TelephonyManager.NETWORK_TYPE_UNKNOWN, "No data network");
        equal(t.phone.getDataState(), TelephonyManager.DATA_DISCONNECTED, "Data disconnected");
        equal(t.phone.getSignalStrength(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID), null, "No signal");
        equal(t.phone.getServiceStateForSlot(0, false, false, STATE.name, null), null, "No service state");
        equal(t.phone.getNetworkCountryIsoForPhone(0), "", "No network country");
        equal(t.phone.getImeiForSlot(0, STATE.name, null), null, "No IMEI");
        equal(t.phone.getDeviceIdWithFeature(STATE.name, null), null, "No device ID");
        equal(t.phone.getTypeAllocationCodeForSlot(0), null, "No TAC");
        equal(t.phone.getSubscriptionCarrierId(1), TelephonyManager.UNKNOWN_CARRIER_ID, "Unknown carrier");
        check(!t.phone.isRadioOnWithFeature(STATE.name, null), "No radio");
        // Without FEATURE_TELEPHONY AOSP publishes no phone, iphonesubinfo or isub service
        // (PhoneGlobals.onCreate returns early), so the managers answer from their null-service
        // paths: no permission check, no SecurityException, the same placeholder values, and
        // IllegalStateException where the manager throws it.
        for (App app : new App[] {PLAIN, STATE, PRIVILEGED}) {
            callers.current = app;
            String n = app.name;
            String who = " (" + n + ")";
            equal(t.phone.getDataNetworkTypeForSubscriber(1, n, null), TelephonyManager.NETWORK_TYPE_UNKNOWN,
                    "Data network type without phone service" + who);
            equal(t.phone.getDataNetworkType(n, null), TelephonyManager.NETWORK_TYPE_UNKNOWN,
                    "Default data network type without phone service" + who);
            equal(t.phone.getNetworkTypeForSubscriber(1, n, null), TelephonyManager.NETWORK_TYPE_UNKNOWN,
                    "Network type without phone service" + who);
            equal(t.phone.getVoiceNetworkTypeForSubscriber(1, n, null), TelephonyManager.NETWORK_TYPE_UNKNOWN,
                    "Voice network type without phone service" + who);
            check(!t.phone.isRadioOnWithFeature(n, null), "Radio off without phone service" + who);
            equal(t.phone.getLine1NumberForDisplay(1, n, null), null, "Display number without phone service" + who);
            equal(t.phone.getDeviceIdWithFeature(n, null), null, "Device ID without phone service" + who);
            equal(t.phone.getImeiForSlot(0, n, null), null, "IMEI without phone service" + who);
            equal(t.phone.getDeviceSoftwareVersionForSlot(0, n, null), null, "IMEISV without phone service" + who);
            equal(t.phone.getServiceStateForSlot(0, false, false, n, null), null,
                    "Service state without phone service" + who);
            throwsType(IllegalStateException.class, () -> t.phone.getPrimaryImei(n, null),
                    "TelephonyManager.getPrimaryImei's null-service exception" + who);
            equal(t.phoneSubInfo.getDeviceIdForPhone(0, n, null), null, "Device ID (subinfo)" + who);
            equal(t.phoneSubInfo.getDeviceIdWithFeature(n, null), null, "Default device ID (subinfo)" + who);
            equal(t.phoneSubInfo.getImeiForSubscriber(1, n, null), null, "IMEI (subinfo)" + who);
            equal(t.phoneSubInfo.getSubscriberIdForSubscriber(1, n, null), null, "IMSI" + who);
            equal(t.phoneSubInfo.getIccSerialNumberForSubscriber(1, n, null), null, "ICCID" + who);
            equal(t.phoneSubInfo.getIccSerialNumberWithFeature(n, null), null, "Default ICCID" + who);
            equal(t.phoneSubInfo.getLine1NumberForSubscriber(1, n, null), null, "Line 1 number" + who);
            equal(t.phoneSubInfo.getLine1Number(n, null), null, "Default line 1 number" + who);
            check(t.subscriptions.getActiveSubscriptionInfoList(n, null, false).isEmpty(),
                    "Empty subscription list without isub" + who);
            equal(t.subscriptions.getActiveSubInfoCount(n, null, false), 0, "Zero subscriptions without isub" + who);
            equal(t.subscriptions.getActiveSubscriptionInfo(1, n, null), null, "No subscription info" + who);
            equal(t.subscriptions.getActiveSubscriptionInfoForSimSlotIndex(0, n, null), null,
                    "No subscription in slot 0" + who);
            check(t.subscriptions.getAllSubInfoList(n, null).isEmpty(), "No subscriptions at all" + who);
            check(t.subscriptions.getAvailableSubscriptionInfoList(n, null).isEmpty(),
                    "No available subscriptions" + who);
            check(!t.subscriptions.isActiveSubId(1, n, null), "No active subscription ID" + who);
            throwsType(IllegalStateException.class, () -> t.subscriptions.getPhoneNumber(1,
                    SubscriptionManager.PHONE_NUMBER_SOURCE_UICC, n, null),
                    "SubscriptionManager.getPhoneNumber's null-service exception" + who);
            throwsType(IllegalStateException.class,
                    () -> t.subscriptions.getPhoneNumberFromFirstAvailableSource(1, n, null),
                    "getPhoneNumber(int)'s null-service exception" + who);
        }
        callers.current = STATE;
        throwsType(IllegalStateException.class, () -> { t.phone.requestCellInfoUpdate(1, null, STATE.name, null);
                return null; }, "requestCellInfoUpdate's null-service exception from target S");
        throwsType(IllegalStateException.class, () -> { t.phone.requestCellInfoUpdateWithWorkSource(1, null,
                STATE.name, null, null); return null; }, "and with a work source");
        callers.current = LEGACY;
        t.phone.requestCellInfoUpdate(1, null, LEGACY.name, null);
        check(true, "requestCellInfoUpdate returns silently below target S");
        callers.current = STATE;
        TelephonyHost.callers = callers;
        noModemSweep(t);
        callers.current = STATE;
        equal(t.subscriptions.getActiveSubInfoCountMax(), 0, "No modem slots");
        equal(t.subscriptions.getDefaultSubId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID, "No default");
        equal(t.subscriptions.getActiveSubIdList(true).length, 0, "No active IDs");
        equal(t.subscriptions.getPhoneId(17), SubscriptionManager.DEFAULT_PHONE_INDEX, "Unmapped sub sentinel");
        equal(t.subscriptions.getPhoneId(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID),
                SubscriptionManager.INVALID_PHONE_INDEX, "No default phone");
        Listener listener = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name,
                null, listener, new int[] {TelephonyCallback.EVENT_SERVICE_STATE_CHANGED,
                        TelephonyCallback.EVENT_SIGNAL_STRENGTHS_CHANGED}, true);
        check(listener.events.isEmpty(), "Zero phones: no initial notifications");
        App modern = new App("com.example.modern", 10114, 31);
        callers.current = modern;
        denied(() -> { t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                modern.name, null, new Listener(), new int[] {TelephonyCallback.EVENT_CALL_STATE_CHANGED}, true);
                return null; }, "telephony.registry keeps READ_PHONE_STATE for call state without a modem");
        callers.current = STATE;
        SubscriptionsListener subscriptions = new SubscriptionsListener();
        t.registry.addOnSubscriptionsChangedListener(STATE.name, null, subscriptions);
        equal(subscriptions.calls, 1, "Subscription listeners hear the initial (empty) state");
        t.registry.addOnSubscriptionsChangedListener(STATE.name, null, subscriptions);
        equal(t.registry.listenerCount(), 2, "One record per listener binder");
        equal(subscriptions.remote.recipients.size(), 1, "One death link per listener binder");
        // Without a modem no event ever reaches these listeners, so a dead app's records
        // would stay in system_server forever unless its death drops them.
        listener.remote.die();
        subscriptions.remote.die();
        equal(t.registry.listenerCount(), 0, "Dead listeners are dropped");
        Listener removed = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name,
                null, removed, new int[] {TelephonyCallback.EVENT_SERVICE_STATE_CHANGED}, false);
        equal(removed.remote.recipients.size(), 1, "Listening links to death");
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name,
                null, removed, new int[0], false);
        equal(t.registry.listenerCount(), 0, "LISTEN_NONE removes the record");
        equal(removed.remote.recipients.size(), 0, "and unlinks its death recipient");
    }

    /** Without a modem: what TelephonyManager/SubscriptionManager answer with no service. */
    static Fixture callers;

    static void noModemSweep(TelephonyService.Endpoints t) throws Exception {
        String n = STATE.name;
        equal(t.phone.getAllCellInfo(n, null), null, "getAllCellInfo: null without the service");
        android.telephony.CellIdentity identity = t.phone.getCellLocation(n, null);
        check(identity != null && identity.asCellLocation().isEmpty(),
                "getCellLocation: an empty identity, which the manager turns into null (not an NPE)");
        throwsType(IllegalStateException.class, () -> t.phone.isDataEnabledForReason(1, 0) ? 1 : 0,
                "isDataEnabledForReason throws without the service");
        check(!t.phone.isDataEnabled(1) && !t.phone.isDataRoamingEnabled(1), "Data switches false");
        check(!t.phone.hasIccCardUsingSlotIndex(0), "No ICC card");
        check(!t.phone.isModemEnabledForSlot(0, n, null), "No modem enabled");
        equal(t.phone.getCardIdForDefaultEuicc(1, n), TelephonyManager.UNINITIALIZED_CARD_ID, "UNINITIALIZED_CARD_ID");
        check(t.phone.getUiccCardsInfo(n).isEmpty(), "No UICC cards");
        IntResult purchase = new IntResult();
        t.phone.purchasePremiumCapability(1, purchase, 1);
        equal(purchase.results, List.of(TelephonyManager.PURCHASE_PREMIUM_CAPABILITY_RESULT_REQUEST_FAILED),
                "purchasePremiumCapability fails as the manager fails it");
        throwsType(IllegalStateException.class, () -> t.phone.isPremiumCapabilityAvailableForPurchase(1, 1) ? 1 : 0,
                "isPremiumCapabilityAvailableForPurchase throws");
        throwsType(IllegalStateException.class, () -> { t.phone.getSlicingConfig(new Receiver()); return null; },
                "getNetworkSlicingConfiguration throws");
        throwsType(IllegalStateException.class, () -> { t.phone.uploadCallComposerPicture(1, n, "image/jpeg",
                null, new Receiver()); return null; }, "uploadCallComposerPicture throws");
        GbaCallback gba = new GbaCallback();
        t.phone.bootstrapAuthenticationRequest(1, 0, null, null, false, gba);
        equal(gba.reasons, List.of(TelephonyManager.GBA_FAILURE_REASON_FEATURE_NOT_READY), "GBA not ready");
        equal(t.phone.requestNetworkScan(1, false, null, null, null, n, null),
                android.telephony.TelephonyScanManager.INVALID_SCAN_ID, "No network scan");
        throwsType(IllegalStateException.class, () -> t.phone.isEmergencyNumber("112", true) ? 1 : 0,
                "isEmergencyNumber throws without the service");
        throwsType(IllegalStateException.class, () -> t.phone.getEmergencyNumberList(n, null),
                "getEmergencyNumberList throws without the service");
        throwsType(IllegalStateException.class, () -> t.subscriptions.getSubscriptionsInGroup(
                new android.os.ParcelUuid(java.util.UUID.randomUUID()), n, null), "getSubscriptionsInGroup throws");
        Receiver ussd = new Receiver();
        t.phone.handleUssdRequest(1, "*100#", ussd);
        check(ussd.codes.isEmpty(), "USSD is dropped silently without the service");
        callers.current = PLAIN;
        for (String value : subInfoGetters(t, PLAIN.name)) {
            equal(value, null, "No iphonesubinfo: null without a permission check");
        }
    }

    /** Every remaining IPhoneSubInfo getter, for one caller. */
    static List<String> subInfoGetters(TelephonyService.Endpoints t, String n) throws Exception {
        List<String> values = new ArrayList<>();
        values.add(t.phoneSubInfo.getGroupIdLevel1ForSubscriber(1, n, null));
        values.add(t.phoneSubInfo.getGroupIdLevel2ForSubscriber(1, n, null));
        values.add(t.phoneSubInfo.getLine1AlphaTag(n, null));
        values.add(t.phoneSubInfo.getVoiceMailNumber(n, null));
        values.add(t.phoneSubInfo.getVoiceMailAlphaTag(n, null));
        values.add(t.phoneSubInfo.getDeviceSvn(n, null));
        values.add(t.phoneSubInfo.getNaiForSubscriber(1, n, null));
        values.add(t.phoneSubInfo.getMsisdn(n, null));
        values.add(t.phoneSubInfo.getDeviceId(n));
        values.add(t.phoneSubInfo.getSubscriberId(n));
        values.add(t.phoneSubInfo.getIccSerialNumber(n));
        return values;
    }

    static final App NETSTATE = new App("com.example.netstate", 10120, 34, TelephonyService.ACCESS_NETWORK_STATE);
    static final App COARSE_ONLY = new App("com.example.coarse", 10121, 34, TelephonyService.COARSE);
    static final App CALLER = new App("com.example.caller", 10122, 34, TelephonyService.CALL_PHONE);
    static final App UPSELL = new App("com.example.upsell", 10123, 34, TelephonyService.READ_BASIC_PHONE_STATE,
            TelephonyService.INTERNET);
    static final App MODIFY = new App("com.example.modify", 10124, 34, TelephonyService.MODIFY_PHONE_STATE);
    static final App PRECISE = new App("com.example.precise", 10125, 34, TelephonyService.READ_PRECISE_PHONE_STATE);

    /**
     * With a modem (features declared): every swept ITelephony/ISub answer agrees with the
     * modem and every callback API answers.
     */
    static void modemContracts() throws Exception {
        Source source = new Source();
        ModemSpec spec = new ModemSpec();
        source.set(spec);
        TelephonyService.Cellular cellular = new TelephonyService.Cellular(source,
                WindowsCellular.parse(source.text), new Properties());
        Fixture callers = new Fixture();
        TelephonyService.Endpoints t = new TelephonyService.Endpoints(cellular, callers);

        // Phone type, SIM card, radio and modem state agree with the modem.
        equal(WindowsCellular.phoneType(cellular.modem()), TelephonyManager.PHONE_TYPE_GSM, "Phone type GSM");
        ModemSpec cdmaClass = new ModemSpec();
        cdmaClass.cellular = WindowsCellular.CLASS_CDMA;
        Source cdmaSource = new Source();
        cdmaSource.set(cdmaClass);
        equal(WindowsCellular.phoneType(WindowsCellular.parse(cdmaSource.text).primary()),
                TelephonyManager.PHONE_TYPE_GSM, "Android 16 reports GSM for any phone");
        equal(t.phone.getSimStateForSlotIndex(SubscriptionManager.DEFAULT_SIM_SLOT_INDEX),
                TelephonyManager.SIM_STATE_LOADED, "The default slot is phone 0's");
        check(t.phone.hasIccCardUsingSlotIndex(0) && t.phone.hasIccCard()
                && t.phone.hasIccCardUsingSlotIndex(SubscriptionManager.DEFAULT_SIM_SLOT_INDEX), "ICC card present");
        check(!t.phone.hasIccCardUsingSlotIndex(1), "No second slot");
        callers.current = STATE;
        check(t.phone.isRadioOn(STATE.name) && t.phone.isRadioOnForSubscriber(1, STATE.name)
                && t.phone.isRadioOnForSubscriberWithFeature(1, STATE.name, null), "isRadioOn variants agree");
        callers.current = BASIC;
        check(t.phone.isModemEnabledForSlot(0, BASIC.name, null), "Modem enabled");
        check(!t.phone.isModemEnabledForSlot(1, BASIC.name, null), "No modem in slot 1");
        callers.current = PLAIN;
        denied(() -> t.phone.isModemEnabledForSlot(0, PLAIN.name, null), "isModemEnabledForSlot needs permission");
        equal(t.phone.getCardIdForDefaultEuicc(1, PLAIN.name), TelephonyManager.UNSUPPORTED_CARD_ID, "No eUICC");

        // UICC cards.
        denied(() -> t.phone.getUiccCardsInfo(PLAIN.name), "Card info needs permission");
        callers.current = PRIVILEGED;
        List<android.telephony.UiccCardInfo> cards = t.phone.getUiccCardsInfo(PRIVILEGED.name);
        check(cards.size() == 1 && !cards.get(0).isEuicc() && cards.get(0).isRemovable(), "One removable UICC");
        equal(cards.get(0).getPorts().iterator().next().getIccId(), "8997201000000000001", "Privileged ICCID");
        callers.current = BASIC;
        equal(t.phone.getUiccCardsInfo(BASIC.name).get(0).getPorts().iterator().next().getIccId(),
                "FFFFFFFFFFFFFFFFFFFF", "READ_BASIC_PHONE_STATE gets a sanitized card (ICCID redacted)");

        // Data switches never contradict getDataState.
        callers.current = PLAIN;
        denied(() -> t.phone.isDataEnabled(1), "isDataEnabled needs a permission");
        denied(() -> t.phone.isDataEnabledForReason(1, 0), "isDataEnabledForReason needs a permission");
        denied(() -> t.phone.isDataRoamingEnabled(1), "isDataRoamingEnabled needs a permission");
        callers.current = NETSTATE;
        int[][] states = {{WindowsCellular.REGISTER_HOME, WindowsCellular.ACTIVATION_ACTIVATED},
                {WindowsCellular.REGISTER_HOME, WindowsCellular.ACTIVATION_DEACTIVATED},
                {WindowsCellular.REGISTER_ROAMING, WindowsCellular.ACTIVATION_ACTIVATED},
                {WindowsCellular.REGISTER_ROAMING, WindowsCellular.ACTIVATION_DEACTIVATED},
                {WindowsCellular.REGISTER_SEARCHING, WindowsCellular.ACTIVATION_NONE}};
        for (int[] state : states) {
            spec.register = state[0];
            spec.activation = state[1];
            source.set(spec);
            cellular.refresh();
            boolean connected = t.phone.getDataStateForSubId(1) == TelephonyManager.DATA_CONNECTED;
            boolean roaming = state[0] == WindowsCellular.REGISTER_ROAMING;
            String what = " (register " + state[0] + ", activation " + state[1] + ")";
            check(t.phone.isDataEnabled(1) && t.phone.isDataEnabledForReason(1,
                    TelephonyManager.DATA_ENABLED_REASON_USER) && t.phone.isUserDataEnabled(1),
                    "Data enabled with an active subscription" + what);
            check(!connected || t.phone.isDataEnabled(1), "Connected implies enabled" + what);
            equal(t.phone.isDataRoamingEnabled(1), roaming && connected,
                    "Roaming enabled only when observed connected while roaming" + what);
        }
        spec.register = WindowsCellular.REGISTER_HOME;
        spec.activation = WindowsCellular.ACTIVATION_ACTIVATED;
        source.set(spec);
        cellular.refresh();

        // Cell information: AOSP's location rules; Windows reports no cells.
        callers.current = PLAIN;
        denied(() -> t.phone.getAllCellInfo(PLAIN.name, null), "getAllCellInfo needs location");
        denied(() -> t.phone.getCellLocation(PLAIN.name, null), "getCellLocation needs location");
        denied(() -> { t.phone.requestCellInfoUpdate(1, new CellInfoCallback(), PLAIN.name, null); return null; },
                "requestCellInfoUpdate needs location");
        callers.current = COARSE_ONLY;
        denied(() -> { t.phone.requestCellInfoUpdate(1, new CellInfoCallback(), COARSE_ONLY.name, null); return null; },
                "requestCellInfoUpdate needs fine location at every target");
        callers.current = LOCATED;
        check(t.phone.getAllCellInfo(LOCATED.name, null).isEmpty(), "No cells reported");
        check(t.phone.getCellLocation(LOCATED.name, null).asCellLocation().isEmpty(), "Empty cell location");
        CellInfoCallback cb = new CellInfoCallback();
        t.phone.requestCellInfoUpdate(1, cb, LOCATED.name, null);
        t.phone.requestCellInfoUpdateWithWorkSource(1, cb, LOCATED.name, null, null);
        String modemError = "error:" + TelephonyManager.CellInfoCallback.ERROR_MODEM_ERROR + ":"
                + "com.android.internal.telephony.CommandException";
        equal(cb.results, List.of(modemError, modemError), "requestCellInfoUpdate answers ERROR_MODEM_ERROR");
        callers.location = false;
        CellInfoCallback soft = new CellInfoCallback();
        t.phone.requestCellInfoUpdate(1, soft, LOCATED.name, null);
        equal(soft.results, List.of("cells:0"), "Location off: a soft denial answers an empty list");
        check(t.phone.getAllCellInfo(LOCATED.name, null).isEmpty(), "and getAllCellInfo an empty list");
        callers.location = true;

        // Callback APIs always answer.
        callers.current = PLAIN;
        denied(() -> { t.phone.handleUssdRequest(1, "*100#", new Receiver()); return null; }, "USSD needs CALL_PHONE");
        callers.current = CALLER;
        Receiver ussd = new Receiver();
        t.phone.handleUssdRequest(1, "*100#", ussd);
        equal(ussd.codes, List.of(TelephonyManager.USSD_RETURN_FAILURE), "USSD fails as unsupported");
        android.telephony.UssdResponse response = ussd.data.get(0).getParcelable(TelephonyManager.USSD_RESPONSE,
                android.telephony.UssdResponse.class);
        equal(response.getUssdRequest(), "*100#", "The failure carries the request");
        Receiver invalid = new Receiver();
        t.phone.handleUssdRequest(SubscriptionManager.INVALID_SUBSCRIPTION_ID, "*100#", invalid);
        check(invalid.codes.isEmpty(), "Invalid subscription: dropped, as AOSP drops it");
        callers.current = STATE;
        denied(() -> { t.phone.getCarrierRestrictionStatus(new IntResult(), STATE.name); return null; },
                "No caller is allow-listed for carrier restriction status");
        callers.current = PLAIN;
        denied(() -> { t.phone.getCarrierRestrictionStatus(new IntResult(), PLAIN.name); return null; },
                "Carrier restriction status needs a permission");
        denied(() -> { t.phone.uploadCallComposerPicture(1, PLAIN.name, "image/jpeg", null, new Receiver());
                return null; }, "Only the dialer role holder uploads call composer pictures");
        callers.current = UPSELL;
        IntResult purchase = new IntResult();
        t.phone.purchasePremiumCapability(1, purchase, 1);
        equal(purchase.results, List.of(TelephonyManager.PURCHASE_PREMIUM_CAPABILITY_RESULT_FEATURE_NOT_SUPPORTED),
                "No slicing upsell");
        check(!t.phone.isPremiumCapabilityAvailableForPurchase(1, 1), "Nothing to purchase");
        callers.current = BASIC;
        denied(() -> { t.phone.purchasePremiumCapability(1, new IntResult(), 1); return null; },
                "Purchasing needs INTERNET");
        callers.current = PLAIN;
        denied(() -> t.phone.isPremiumCapabilityAvailableForPurchase(1, 1) ? 1 : 0,
                "isPremiumCapabilityAvailableForPurchase needs READ_BASIC_PHONE_STATE");
        callers.current = STATE;
        denied(() -> { t.phone.getSlicingConfig(new Receiver()); return null; }, "Slicing config is privileged");
        callers.current = PRIVILEGED;
        Receiver slicing = new Receiver();
        t.phone.getSlicingConfig(slicing);
        equal(slicing.codes, List.of(TelephonyManager.NetworkSlicingException.ERROR_MODEM_ERROR),
                "No slicing configuration from the modem");
        callers.current = PLAIN;
        denied(() -> { t.phone.bootstrapAuthenticationRequest(1, 0, null, null, false, new GbaCallback());
                return null; }, "GBA needs a privileged permission");
        callers.current = MODIFY;
        GbaCallback gba = new GbaCallback();
        t.phone.bootstrapAuthenticationRequest(1, 0, null, null, false, gba);
        equal(gba.reasons, List.of(TelephonyManager.GBA_FAILURE_REASON_FEATURE_NOT_SUPPORTED), "No GBA");
        equal(t.phone.requestNetworkScan(1, false, null, null, null, MODIFY.name, null),
                android.telephony.TelephonyScanManager.INVALID_SCAN_ID, "No network scan");
        callers.current = PLAIN;
        denied(() -> t.phone.requestNetworkScan(1, false, null, null, null, PLAIN.name, null),
                "Network scans need MODIFY_PHONE_STATE");

        // Remaining iphonesubinfo getters: PhoneSubInfoController's checks.
        callers.current = PLAIN;
        for (Call call : new Call[] {
                () -> t.phoneSubInfo.getGroupIdLevel1ForSubscriber(1, PLAIN.name, null),
                () -> t.phoneSubInfo.getGroupIdLevel2ForSubscriber(1, PLAIN.name, null),
                () -> t.phoneSubInfo.getLine1AlphaTag(PLAIN.name, null),
                () -> t.phoneSubInfo.getVoiceMailNumber(PLAIN.name, null),
                () -> t.phoneSubInfo.getVoiceMailAlphaTag(PLAIN.name, null),
                () -> t.phoneSubInfo.getDeviceSvn(PLAIN.name, null),
                () -> t.phoneSubInfo.getNaiForSubscriber(1, PLAIN.name, null),
                () -> t.phoneSubInfo.getMsisdn(PLAIN.name, null),
                () -> t.phoneSubInfo.getSubscriberId(PLAIN.name),
                () -> t.phoneSubInfo.getIccSerialNumber(PLAIN.name),
                () -> t.phoneSubInfo.getDeviceId(PLAIN.name)}) {
            denied(call, "iphonesubinfo getter needs its permission");
        }
        callers.current = REVOKED;
        equal(t.phoneSubInfo.getGroupIdLevel1ForSubscriber(1, REVOKED.name, null), null,
                "GID1: a revoked app op gives null");
        callers.current = STATE;
        equal(t.phoneSubInfo.getGroupIdLevel1ForSubscriber(1, STATE.name, null), null,
                "GID1: Windows does not report it");
        equal(t.phoneSubInfo.getVoiceMailNumber(STATE.name, null), null, "No voicemail number");
        denied(() -> t.phoneSubInfo.getNaiForSubscriber(1, STATE.name, null), "NAI is a subscriber identifier");
        denied(() -> t.phoneSubInfo.getMsisdn(STATE.name, null), "MSISDN needs phone-number access from R");
        callers.current = NUMBERS;
        equal(t.phoneSubInfo.getMsisdn(NUMBERS.name, null), "+972501234567", "MSISDN with READ_PHONE_NUMBERS");
        callers.current = PRIVILEGED;
        equal(t.phoneSubInfo.getSubscriberId(PRIVILEGED.name), "425010123456789", "Deprecated IMSI overload");
        equal(t.phoneSubInfo.getIccSerialNumber(PRIVILEGED.name), "8997201000000000001", "Deprecated ICCID overload");
        equal(t.phoneSubInfo.getDeviceId(PRIVILEGED.name), "356938035643809", "Deprecated device ID overload");

        // Emergency numbers: the ECC fallback of EmergencyNumberTracker.
        callers.current = PLAIN;
        check(t.phone.isEmergencyNumber("112", true) && t.phone.isEmergencyNumber("911", true),
                "112 and 911 with a SIM");
        check(!t.phone.isEmergencyNumber("000", true) && !t.phone.isEmergencyNumber(null, true)
                && !t.phone.isEmergencyNumber("sip:112@example.com", true), "Only the SIM list");
        denied(() -> t.phone.isEmergencyNumber("112", false) ? 1 : 0, "Potential matching is privileged");
        denied(() -> t.phone.getEmergencyNumberList(PLAIN.name, null), "The list needs READ_PHONE_STATE");
        callers.current = STATE;
        @SuppressWarnings("unchecked")
        Map<Integer, List<android.telephony.emergency.EmergencyNumber>> list =
                (Map<Integer, List<android.telephony.emergency.EmergencyNumber>>) t.phone.getEmergencyNumberList(
                        STATE.name, null);
        equal(list.keySet(), Set.of(1), "Keyed by the subscription");
        equal(list.get(1).size(), 2, "112 and 911");
        callers.current = PRIVILEGED;
        check(t.subscriptions.getSubscriptionsInGroup(new android.os.ParcelUuid(java.util.UUID.randomUUID()),
                PRIVILEGED.name, null).isEmpty(), "No subscription groups");
        callers.current = STATE;
        denied(() -> t.subscriptions.getSubscriptionsInGroup(new android.os.ParcelUuid(java.util.UUID.randomUUID()),
                STATE.name, null), "Groups need identifier access from T");
        check(t.subscriptions.getOpportunisticSubscriptions(STATE.name, null).isEmpty(), "No opportunistic subscriptions");

        // telephony.registry: the initial values TelephonyRegistry gives a phone listener.
        callers.current = STATE;
        Listener listener = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name, null,
                listener, new int[] {TelephonyCallback.EVENT_DISPLAY_INFO_CHANGED,
                        TelephonyCallback.EVENT_ACTIVE_DATA_SUBSCRIPTION_ID_CHANGED,
                        TelephonyCallback.EVENT_DATA_ACTIVITY_CHANGED,
                        TelephonyCallback.EVENT_USER_MOBILE_DATA_STATE_CHANGED}, true);
        equal(listener.events, List.of("activity:0", "display", "activeData:1", "userData:true"),
                "Initial data activity, display info, active data subscription and user data state");
        equal(listener.display.getNetworkType(), TelephonyManager.NETWORK_TYPE_LTE, "Display network type");
        equal(listener.display.getOverrideNetworkType(),
                android.telephony.TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NONE, "No override on plain LTE");
        callers.current = PRECISE;
        Listener precise = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, PRECISE.name, null,
                precise, new int[] {TelephonyCallback.EVENT_DATA_ENABLED_CHANGED}, true);
        equal(precise.events, List.of("dataEnabled:true:" + TelephonyManager.DATA_ENABLED_REASON_USER),
                "Initial data enabled state");
        callers.current = LOCATED;
        Listener located = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, LOCATED.name, null,
                located, new int[] {TelephonyCallback.EVENT_CELL_INFO_CHANGED,
                        TelephonyCallback.EVENT_CELL_LOCATION_CHANGED}, true);
        equal(located.events, List.of("cellLocation:null", "cellInfo:0"), "Initial cell location and cell info");
        Listener renounced = new Listener();
        t.registry.listenWithEventList(true, true, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, LOCATED.name, null,
                renounced, new int[] {TelephonyCallback.EVENT_CELL_INFO_CHANGED}, true);
        check(renounced.events.isEmpty(), "Renounced location: no cell info");

        // Changes: 5G NSA, then SIM removal.
        listener.events.clear();
        precise.events.clear();
        spec.dataClass = WindowsCellular.DATA_LTE | WindowsCellular.DATA_5G_NSA;
        source.set(spec);
        check(cellular.refresh(), "Data class change observed");
        equal(listener.events, List.of("display"), "Display info change delivered");
        equal(listener.display.getOverrideNetworkType(),
                android.telephony.TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NR_NSA, "5G NSA is LTE overridden as NR_NSA");
        listener.events.clear();
        spec.ready = WindowsCellular.READY_SIM_NOT_INSERTED;
        spec.imsi = null; spec.iccid = null; spec.number = null; spec.home = null; spec.homeName = null;
        spec.register = WindowsCellular.REGISTER_SEARCHING;
        spec.activation = WindowsCellular.ACTIVATION_NONE;
        source.set(spec);
        check(cellular.refresh(), "SIM removal observed");
        check(listener.events.contains("activeData:" + SubscriptionManager.INVALID_SUBSCRIPTION_ID)
                && listener.events.contains("userData:false"), "Subscription and data state changes delivered");
        equal(precise.events, List.of("dataEnabled:false:" + TelephonyManager.DATA_ENABLED_REASON_USER),
                "Data enabled follows the subscription");
        check(!t.phone.hasIccCardUsingSlotIndex(0), "No card after removal");
        callers.current = NETSTATE;
        check(!t.phone.isDataEnabled(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID), "No subscription: data disabled");
        check(t.phone.isEmergencyNumber("000", true), "Without a SIM the 3GPP no-SIM list applies");
    }

    static void modem() throws Exception {
        Source source = new Source();
        ModemSpec spec = new ModemSpec();
        source.set(spec);
        WindowsCellular.Snapshot boot = WindowsCellular.parse(source.text);
        check(boot != null && boot.modems.size() == 1, "Modem snapshot parses");
        Set<String> features = features(boot);
        equal(features, Set.of(PackageManager.FEATURE_TELEPHONY, PackageManager.FEATURE_TELEPHONY_SUBSCRIPTION,
                PackageManager.FEATURE_TELEPHONY_RADIO_ACCESS, PackageManager.FEATURE_TELEPHONY_DATA,
                PackageManager.FEATURE_TELEPHONY_GSM), "Data-only GSM modem features");
        Properties properties = new Properties();
        TelephonyService.Cellular cellular = new TelephonyService.Cellular(source, boot, properties);
        Fixture callers = new Fixture();
        TelephonyService.Endpoints t = new TelephonyService.Endpoints(cellular, callers);
        equal(properties.simStates, List.of(TelephonyManager.SIM_STATE_LOADED), "Properties published at boot");
        equal(cellular.subId(), 1, "First ICCID gets subscription 1");

        callers.current = STATE;
        equal(t.phone.getSimStateForSlotIndex(0), TelephonyManager.SIM_STATE_LOADED, "SIM loaded");
        equal(t.phone.getSimStateForSlotIndex(1), TelephonyManager.SIM_STATE_UNKNOWN, "No second slot");
        equal(t.phone.getDataNetworkTypeForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                STATE.name, null), TelephonyManager.NETWORK_TYPE_LTE, "LTE");
        equal(t.phone.getNetworkTypeForSubscriber(1, STATE.name, null), TelephonyManager.NETWORK_TYPE_LTE,
                "Network type follows data for target > Q");
        equal(t.phone.getDataStateForSubId(1), TelephonyManager.DATA_CONNECTED, "Data connected");
        equal(t.phone.getNetworkCountryIsoForPhone(0), "il", "Network country from MCC 425");
        check(t.phone.isRadioOnWithFeature(STATE.name, null), "Radio on");
        SignalStrength signal = t.phone.getSignalStrength(1);
        check(signal != null && signal.getCellSignalStrengths().size() == 1, "One measurement");
        equal(((android.telephony.CellSignalStrengthLte) signal.getCellSignalStrengths().get(0)).getRssi(), -73,
                "CSQ 20 is -73 dBm");
        callers.current = REVOKED;
        equal(t.phone.getDataNetworkTypeForSubscriber(1, REVOKED.name, null),
                TelephonyManager.NETWORK_TYPE_UNKNOWN, "Revoked app op gives placeholder");
        callers.current = PLAIN;
        denied(() -> t.phone.getServiceStateForSlot(0, false, false, PLAIN.name, null), "Service state needs permission");
        denied(() -> t.phone.getDataNetworkTypeForSubscriber(1, PLAIN.name, null), "Network type needs a permission");
        denied(() -> t.phoneSubInfo.getSubscriberIdForSubscriber(1, PLAIN.name, null), "IMSI needs access");
        denied(() -> t.phone.getLine1NumberForDisplay(1, PLAIN.name, null), "Number needs a permission");
        denied(() -> t.subscriptions.getActiveSubInfoCount(PLAIN.name, null, false), "Count needs a permission");
        check(t.subscriptions.getActiveSubscriptionInfoList(PLAIN.name, null, false).isEmpty(),
                "Empty list without permission");
        callers.current = BASIC;
        equal(t.phone.getDataNetworkTypeForSubscriber(1, BASIC.name, null),
                TelephonyManager.NETWORK_TYPE_LTE, "READ_BASIC_PHONE_STATE suffices for the type");
        callers.current = LEGACY;
        equal(t.phoneSubInfo.getSubscriberIdForSubscriber(1, LEGACY.name, null), null,
                "Pre-Q READ_PHONE_STATE gets placeholder null");
        callers.current = PLAIN;
        denied(() -> t.phone.getDataNetworkTypeForSubscriber(1, "com.example.other", null), "Package spoofing");

        // Location-sanitized ServiceState.
        callers.current = STATE;
        ServiceState sanitized = t.phone.getServiceStateForSlot(0, false, false, STATE.name, null);
        equal(sanitized.getDataRegistrationState(), ServiceState.STATE_IN_SERVICE, "Data in service");
        equal(sanitized.getState(), ServiceState.STATE_OUT_OF_SERVICE, "No voice service");
        equal(sanitized.getDataNetworkType(), TelephonyManager.NETWORK_TYPE_LTE, "Data RAT");
        equal(sanitized.getOperatorNumeric(), null, "Operator removed without location");
        callers.current = LOCATED;
        ServiceState full = t.phone.getServiceStateForSlot(0, false, false, LOCATED.name, null);
        equal(full.getOperatorNumeric(), "42501", "Operator with fine location");
        equal(full.getOperatorAlphaLong(), "Partner IL", "Operator name");
        check(!full.getRoaming(), "Home network");
        equal(t.phone.getServiceStateForSlot(0, true, true, LOCATED.name, null).getOperatorNumeric(), null,
                "Renounced location access");
        callers.location = false;
        equal(t.phone.getServiceStateForSlot(0, false, false, LOCATED.name, null).getOperatorNumeric(), null,
                "Location switch off");
        callers.location = true;

        // Device and subscriber identifiers.
        callers.current = STATE;
        denied(() -> t.phone.getImeiForSlot(0, STATE.name, null), "IMEI needs privileged access (target >= Q)");
        denied(() -> t.phoneSubInfo.getIccSerialNumberForSubscriber(1, STATE.name, null), "ICCID needs access");
        callers.current = LEGACY;
        equal(t.phone.getImeiForSlot(0, LEGACY.name, null), null, "Pre-Q placeholder IMEI");
        callers.current = PRIVILEGED;
        equal(t.phone.getImeiForSlot(0, PRIVILEGED.name, null), "356938035643809", "Privileged IMEI");
        equal(t.phone.getPrimaryImei(PRIVILEGED.name, null), "356938035643809", "Primary IMEI");
        equal(t.phone.getDeviceIdWithFeature(PRIVILEGED.name, null), "356938035643809", "Device ID");
        equal(t.phoneSubInfo.getSubscriberIdForSubscriber(1, PRIVILEGED.name, null), "425010123456789", "IMSI");
        equal(t.phoneSubInfo.getIccSerialNumberForSubscriber(1, PRIVILEGED.name, null), "8997201000000000001",
                "ICCID without padding");
        equal(t.phone.getTypeAllocationCodeForSlot(0), "35693803", "TAC needs no permission");
        App identifiers = new App("com.example.ids", 10110, 34)
                .op(AppOpsManager.OPSTR_READ_DEVICE_IDENTIFIERS, AppOpsManager.MODE_ALLOWED);
        callers.current = identifiers;
        equal(t.phone.getImeiForSlot(0, identifiers.name, null), "356938035643809", "READ_DEVICE_IDENTIFIERS app op");

        // Phone numbers.
        callers.current = NUMBERS;
        equal(t.phoneSubInfo.getLine1NumberForSubscriber(1, NUMBERS.name, null), "+972501234567", "Number");
        equal(t.phone.getLine1NumberForDisplay(1, NUMBERS.name, null), null, "No carrier display number");
        App ignored = new App("com.example.ignored", 10111, 34, TelephonyService.READ_PHONE_NUMBERS)
                .op(AppOpsManager.OPSTR_READ_PHONE_NUMBERS, AppOpsManager.MODE_IGNORED);
        callers.current = ignored;
        denied(() -> t.phoneSubInfo.getLine1NumberForSubscriber(1, ignored.name, null),
                "READ_PHONE_NUMBERS with its app op denied, target >= R");
        App q = new App("com.example.q", 10112, 29, TelephonyService.READ_PHONE_STATE);
        callers.current = q;
        equal(t.phoneSubInfo.getLine1NumberForSubscriber(1, q.name, null), "+972501234567",
                "READ_PHONE_STATE reads the number up to Q");
        callers.current = STATE;
        denied(() -> t.phoneSubInfo.getLine1NumberForSubscriber(1, STATE.name, null),
                "READ_PHONE_STATE alone does not read the number from R");
        App sms = new App("com.example.sms", 10113, 34).op(AppOpsManager.OPSTR_WRITE_SMS, AppOpsManager.MODE_ALLOWED);
        callers.current = sms;
        equal(t.phoneSubInfo.getLine1NumberForSubscriber(1, sms.name, null), "+972501234567",
                "The default SMS app (WRITE_SMS app op) reads the number");

        // Subscriptions and identifier redaction.
        callers.current = STATE;
        List<SubscriptionInfo> list = t.subscriptions.getActiveSubscriptionInfoList(STATE.name, null, false);
        equal(list.size(), 1, "One active subscription");
        SubscriptionInfo info = list.get(0);
        equal(info.getSubscriptionId(), 1, "Subscription ID");
        equal(info.getSimSlotIndex(), 0, "Slot 0");
        equal(info.getMccString(), "425", "MCC");
        equal(info.getMncString(), "01", "MNC");
        equal(info.getCountryIso(), "il", "Country");
        equal(info.getIccId(), "", "ICCID redacted");
        equal(info.getNumber(), "", "Number redacted");
        equal(info.getCarrierId(), TelephonyManager.UNKNOWN_CARRIER_ID, "Unknown carrier ID");
        callers.current = NUMBERS;
        info = t.subscriptions.getActiveSubscriptionInfo(1, NUMBERS.name, null);
        equal(info.getNumber(), "+972501234567", "Number visible with READ_PHONE_NUMBERS");
        equal(info.getIccId(), "", "ICCID still redacted");
        callers.current = PRIVILEGED;
        equal(t.subscriptions.getActiveSubscriptionInfoForSimSlotIndex(0, PRIVILEGED.name, null).getIccId(),
                "8997201000000000001", "Privileged ICCID");
        equal(t.subscriptions.getAllSubInfoList(PRIVILEGED.name, null).size(), 1, "All subscriptions");
        callers.current = STATE;
        denied(() -> t.subscriptions.getAllSubInfoList(STATE.name, null), "All subscriptions are privileged");
        throwsType(IllegalArgumentException.class,
                () -> t.subscriptions.getActiveSubscriptionInfoForSimSlotIndex(-5, STATE.name, null), "Bad slot");
        equal(t.subscriptions.getActiveSubInfoCountMax(), 1, "One slot");
        equal(t.subscriptions.getSubId(0), 1, "Slot 0's subscription");
        equal(t.subscriptions.getSlotIndex(1), 0, "Subscription's slot");
        equal(t.subscriptions.getPhoneId(1), 0, "Subscription's phone");
        equal(t.subscriptions.getDefaultDataSubId(), 1, "Default data subscription");
        check(t.subscriptions.isActiveSubId(1, STATE.name, null), "Active");
        callers.current = NUMBERS;
        equal(t.subscriptions.getPhoneNumber(1, SubscriptionManager.PHONE_NUMBER_SOURCE_UICC, NUMBERS.name, null),
                "+972501234567", "UICC number");
        equal(t.subscriptions.getPhoneNumber(1, SubscriptionManager.PHONE_NUMBER_SOURCE_CARRIER, NUMBERS.name,
                null), "", "No carrier number");
        throwsType(IllegalArgumentException.class,
                () -> t.subscriptions.getPhoneNumber(1, 99, NUMBERS.name, null), "Invalid source");
        callers.current = STATE;
        denied(() -> t.subscriptions.getPhoneNumber(1, SubscriptionManager.PHONE_NUMBER_SOURCE_UICC, STATE.name,
                null), "getPhoneNumber needs READ_PHONE_NUMBERS");

        // Listeners: permission checks, initial values and changes.
        App modern = new App("com.example.modern", 10114, 31);
        callers.current = modern;
        Listener callState = new Listener();
        denied(() -> { t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                modern.name, null, callState, new int[] {TelephonyCallback.EVENT_CALL_STATE_CHANGED}, true);
                return null; }, "Call state needs READ_PHONE_STATE from S");
        denied(() -> { t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                modern.name, null, callState,
                new int[] {TelephonyCallback.EVENT_PRECISE_DATA_CONNECTION_STATE_CHANGED}, true);
                return null; }, "Precise events need READ_PRECISE_PHONE_STATE");
        denied(() -> { t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                modern.name, null, callState, new int[] {TelephonyCallback.EVENT_CELL_INFO_CHANGED}, true);
                return null; }, "Cell info needs location");
        callers.current = STATE;
        Listener listener = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name, null,
                listener, new int[] {TelephonyCallback.EVENT_SERVICE_STATE_CHANGED,
                        TelephonyCallback.EVENT_SIGNAL_STRENGTHS_CHANGED,
                        TelephonyCallback.EVENT_DATA_CONNECTION_STATE_CHANGED,
                        TelephonyCallback.EVENT_CALL_STATE_CHANGED}, true);
        equal(listener.events, List.of("service", "call:0", "data:2:13", "signal"), "Initial notifications");
        equal(listener.state.getOperatorNumeric(), null, "Listener service state is sanitized without location");
        SubscriptionsListener subscriptions = new SubscriptionsListener();
        t.registry.addOnSubscriptionsChangedListener(STATE.name, null, subscriptions);
        equal(subscriptions.calls, 1, "Initial subscriptions call");

        Listener dead = new Listener();
        t.registry.listenWithEventList(false, false, SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name, null,
                dead, new int[] {TelephonyCallback.EVENT_SIGNAL_STRENGTHS_CHANGED}, false);
        dead.remote.die();
        listener.events.clear();
        spec.signal = 25;
        spec.activation = WindowsCellular.ACTIVATION_DEACTIVATED;
        source.set(spec);
        check(cellular.refresh(), "Change observed");
        equal(listener.events, List.of("signal", "data:0:13"), "Only changed values are delivered");
        check(dead.events.isEmpty(), "A dead listener's record is gone");
        check(!cellular.refresh(), "An unchanged reading is not a change");

        // SIM removal: subscription ends, properties follow, listeners hear it.
        listener.events.clear();
        spec.ready = WindowsCellular.READY_SIM_NOT_INSERTED;
        spec.imsi = null; spec.iccid = null; spec.number = null; spec.home = null; spec.homeName = null;
        spec.register = WindowsCellular.REGISTER_SEARCHING;
        source.set(spec);
        check(cellular.refresh(), "SIM removal observed");
        equal(cellular.subId(), SubscriptionManager.INVALID_SUBSCRIPTION_ID, "No subscription");
        equal(t.phone.getSimStateForSlotIndex(0), TelephonyManager.SIM_STATE_ABSENT, "SIM absent");
        equal(properties.simStates.get(properties.simStates.size() - 1), TelephonyManager.SIM_STATE_ABSENT,
                "Property follows");
        equal(subscriptions.calls, 2, "Subscription change delivered");
        check(listener.events.contains("service"), "Service state change delivered to default listener");
        equal(t.phone.getDataNetworkTypeForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID, STATE.name, null),
                TelephonyManager.NETWORK_TYPE_UNKNOWN, "No default subscription: no phone for the query");

        // A new SIM gets a new subscription ID.
        spec = new ModemSpec();
        spec.iccid = "8997201000000000002";
        source.set(spec);
        check(cellular.refresh(), "SIM inserted");
        equal(cellular.subId(), 2, "Second ICCID gets subscription 2");

        // Read failures: the last reading survives two failed polls, not three.
        source.text = "status\terror\t80070005\n";
        check(!cellular.refresh() && !cellular.refresh(), "Two failures keep the reading");
        check(cellular.refresh(), "Third failure drops it");
        check(cellular.modem() == null, "No current reading");
        equal(t.phone.getSimStateForSlotIndex(0), TelephonyManager.SIM_STATE_NOT_READY, "Modem unreadable");
        equal(t.phone.getSignalStrength(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID), null,
                "No subscription without a reading");
        // Phone 0 stays (features are fixed at boot); its service state is radio-off.
        callers.current = STATE;
        equal(t.phone.getServiceStateForSlot(0, false, false, STATE.name, null).getState(),
                ServiceState.STATE_POWER_OFF, "Radio unavailable");
    }

    static void translations() {
        equal(WindowsCellular.networkType(WindowsCellular.DATA_5G_SA | WindowsCellular.DATA_LTE),
                TelephonyManager.NETWORK_TYPE_NR, "5G SA");
        equal(WindowsCellular.networkType(WindowsCellular.DATA_5G_NSA), TelephonyManager.NETWORK_TYPE_LTE, "5G NSA");
        equal(WindowsCellular.networkType(WindowsCellular.DATA_HSDPA | WindowsCellular.DATA_HSUPA),
                TelephonyManager.NETWORK_TYPE_HSPA, "HSPA");
        equal(WindowsCellular.networkType(WindowsCellular.DATA_EDGE), TelephonyManager.NETWORK_TYPE_EDGE, "EDGE");
        equal(WindowsCellular.networkType(0x80000000), TelephonyManager.NETWORK_TYPE_UNKNOWN, "Custom class");
        equal(WindowsCellular.rssiDbm(0), -113, "CSQ 0");
        equal(WindowsCellular.rssiDbm(31), -51, "CSQ 31");
        equal(WindowsCellular.rssiDbm(99), android.telephony.CellInfo.UNAVAILABLE, "CSQ unknown");
        equal(WindowsCellular.countryIso("310260"), "us", "US MCC");
        equal(WindowsCellular.countryIso("99999"), "", "Unknown MCC");
        equal(WindowsCellular.plmn("4250"), null, "Short PLMN");
        equal(WindowsCellular.plmn("42A01"), null, "Non-digit PLMN");
        int[][] ready = {{0, TelephonyManager.SIM_STATE_NOT_READY}, {1, TelephonyManager.SIM_STATE_LOADED},
                {2, TelephonyManager.SIM_STATE_ABSENT}, {3, TelephonyManager.SIM_STATE_CARD_IO_ERROR},
                {4, TelephonyManager.SIM_STATE_UNKNOWN}, {6, TelephonyManager.SIM_STATE_PIN_REQUIRED},
                {7, TelephonyManager.SIM_STATE_PUK_REQUIRED}, {8, TelephonyManager.SIM_STATE_ABSENT}};
        for (int[] pair : ready) {
            ModemSpec spec = new ModemSpec();
            spec.ready = pair[0];
            Source source = new Source();
            source.set(spec);
            equal(WindowsCellular.simState(WindowsCellular.parse(source.text).primary()), pair[1],
                    "Ready state " + pair[0]);
        }
        ModemSpec gsm = new ModemSpec();
        gsm.imei = "A1000012345678";
        gsm.cellular = WindowsCellular.CLASS_CDMA;
        Source source = new Source();
        source.set(gsm);
        WindowsCellular.Snapshot cdma = WindowsCellular.parse(source.text);
        equal(WindowsCellular.imei(cdma.primary()), null, "No IMEI for a CDMA device ID");
        check(!features(cdma).contains(PackageManager.FEATURE_TELEPHONY_GSM)
                && !features(cdma).contains(PackageManager.FEATURE_TELEPHONY_CDMA), "No legacy CDMA feature");
        // Malformed native output is rejected as a whole.
        for (String bad : new String[] {"", "status\tok\n", "status\tmaybe\t0\n",
                "status\tok\t0\nmodem\tzz\n", "status\tnone\t0\n" + new ModemSpec().line() + "\n",
                "status\tok\t0\n" + new ModemSpec().line().replace("\t1\t1\t1\t63", "\t1\t1\t1\t-63") + "\n"}) {
            equal(WindowsCellular.parse(bad), null, "Rejects malformed snapshot");
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The services under test are the freshly compiled production sources.
        java.nio.file.Path expected = java.nio.file.Path.of(args[1]).toRealPath();
        for (Class<?> type : new Class<?>[] {WindowsCellular.class, TelephonyService.class}) {
            java.nio.file.Path actual = java.nio.file.Path.of(
                    type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!actual.equals(expected)) throw new AssertionError("Wrong provider: " + type);
        }
        translations();
        noModem();
        modem();
        modemContracts();
        System.out.println("TelephonyHost passed: " + checks + " checks");
    }
}
