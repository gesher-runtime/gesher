/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.telephony.CellInfo;
import android.telephony.CellSignalStrengthGsm;
import android.telephony.CellSignalStrengthLte;
import android.telephony.CellSignalStrengthWcdma;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.TelephonyManager;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Read-only observations of the host's cellular (WWAN) modem through Windows' Mobile
 * Broadband API (compat/net/windows_cellular.cc, built-in libgesher_cellular.so), and
 * their translation into Android telephony values. See docs/telephony.md.
 *
 * <p>This class only reads and translates. It never connects, disconnects, unlocks or
 * reconfigures the modem; Android's permission and identifier rules are applied by
 * {@link TelephonyService}, which is the only consumer.
 */
public final class WindowsCellular {
    private WindowsCellular() {
    }

    /** A source of raw snapshots: Windows in production, a fixture in host tests only. */
    public interface Source {
        /** The native snapshot text, or null if the backend could not be reached. */
        String snapshot();
    }

    private static final Object LOAD_LOCK = new Object();
    private static boolean sLoaded;

    /** The production source: Windows' Mobile Broadband service on this machine. */
    public static final Source WINDOWS = () -> {
        try {
            synchronized (LOAD_LOCK) {
                if (!sLoaded) {
                    System.loadLibrary("gesher_cellular");
                    sLoaded = true;
                }
            }
            return nativeSnapshot();
        } catch (RuntimeException | LinkageError e) {
            // No runtime natives (a host JVM) or no WWAN support: no modem.
            return null;
        }
    };

    private static native String nativeSnapshot();

    // MBN_READY_STATE.
    static final int READY_OFF = 0, READY_INITIALIZED = 1, READY_SIM_NOT_INSERTED = 2,
            READY_BAD_SIM = 3, READY_FAILURE = 4, READY_NOT_ACTIVATED = 5,
            READY_DEVICE_LOCKED = 6, READY_DEVICE_BLOCKED = 7, READY_NO_ESIM_PROFILE = 8;
    // MBN_CELLULAR_CLASS.
    static final int CLASS_GSM = 1, CLASS_CDMA = 2;
    // MBN_REGISTER_STATE.
    static final int REGISTER_NONE = 0, REGISTER_DEREGISTERED = 1, REGISTER_SEARCHING = 2,
            REGISTER_HOME = 3, REGISTER_ROAMING = 4, REGISTER_PARTNER = 5, REGISTER_DENIED = 6;
    // MBN_ACTIVATION_STATE.
    static final int ACTIVATION_NONE = 0, ACTIVATION_ACTIVATED = 1, ACTIVATION_ACTIVATING = 2,
            ACTIVATION_DEACTIVATED = 3, ACTIVATION_DEACTIVATING = 4;
    // MBN_DATA_CLASS bits.
    static final int DATA_GPRS = 0x1, DATA_EDGE = 0x2, DATA_UMTS = 0x4, DATA_HSDPA = 0x8,
            DATA_HSUPA = 0x10, DATA_LTE = 0x20, DATA_5G_NSA = 0x40, DATA_5G_SA = 0x80,
            DATA_1XRTT = 0x10000, DATA_1XEVDO = 0x20000, DATA_1XEVDO_REVA = 0x40000,
            DATA_1XEVDO_REVB = 0x200000;
    /** MBN_RSSI_UNKNOWN and MBN_ERROR_RATE_UNKNOWN. */
    static final int UNKNOWN_99 = 99;
    /** A value the modem did not report. */
    static final int ABSENT = -1;

    /** One WWAN interface as Windows reported it; strings are null when not reported. */
    public static final class Modem {
        public final String interfaceId;
        public final int readyState, cellularClass, voiceClass, dataClasses, smsCaps;
        public final String deviceId, manufacturer, model, firmware;
        public final String subscriberId, iccId;
        public final List<String> numbers;
        public final String homeProviderId, homeProviderName;
        public final int registerState;
        public final String registeredProviderId, registeredProviderName, roamingText;
        public final int currentDataClass, signal, signalError, activation;
        /** The reading as reported, to detect changes. */
        final String source;

        Modem(String[] f) {
            source = String.join("\t", f);
            interfaceId = text(f[1]);
            readyState = number(f[2]);
            cellularClass = number(f[3]);
            voiceClass = number(f[4]);
            dataClasses = number(f[5]);
            smsCaps = number(f[6]);
            deviceId = text(f[7]);
            manufacturer = text(f[8]);
            model = text(f[9]);
            firmware = text(f[10]);
            subscriberId = text(f[11]);
            iccId = text(f[12]);
            numbers = numbers(f[13]);
            homeProviderId = text(f[14]);
            homeProviderName = text(f[15]);
            registerState = number(f[16]);
            registeredProviderId = text(f[17]);
            registeredProviderName = text(f[18]);
            roamingText = text(f[19]);
            currentDataClass = number(f[20]);
            signal = number(f[21]);
            signalError = number(f[22]);
            activation = number(f[23]);
        }

        boolean simReady() { return readyState == READY_INITIALIZED; }

        boolean registered() {
            return readyState == READY_INITIALIZED && (registerState == REGISTER_HOME
                    || registerState == REGISTER_ROAMING || registerState == REGISTER_PARTNER);
        }

        boolean roaming() {
            return registered() && (registerState == REGISTER_ROAMING
                    || registerState == REGISTER_PARTNER);
        }
    }

    /** The result of one read. */
    public static final class Snapshot {
        /** "ok", "none" (no WWAN service/interface) or "error". */
        public final String status;
        public final List<Modem> modems;

        Snapshot(String status, List<Modem> modems) {
            this.status = status;
            this.modems = Collections.unmodifiableList(modems);
        }

        /** The modem Android sees as phone 0, or null. */
        public Modem primary() { return modems.isEmpty() ? null : modems.get(0); }
    }

    private static final int MODEM_FIELDS = 24;

    /** Parses native text; null when malformed (never a partial modem). */
    static Snapshot parse(String raw) {
        if (raw == null) return null;
        String[] lines = raw.split("\n");
        if (lines.length == 0) return null;
        String[] status = lines[0].split("\t", -1);
        if (status.length != 3 || !status[0].equals("status")) return null;
        if (!status[1].equals("ok") && !status[1].equals("none") && !status[1].equals("error")) {
            return null;
        }
        List<Modem> modems = new ArrayList<>();
        try {
            for (int i = 1; i < lines.length; i++) {
                if (lines[i].isEmpty()) continue;
                String[] fields = lines[i].split("\t", -1);
                if (fields.length != MODEM_FIELDS || !fields[0].equals("modem")) return null;
                Modem modem = new Modem(fields);
                if (modem.interfaceId == null || modem.interfaceId.isEmpty()) return null;
                modems.add(modem);
            }
        } catch (IllegalArgumentException e) {
            return null;
        }
        if (!status[1].equals("ok") && !modems.isEmpty()) return null;
        // Interfaces in a stable order, so phone 0 does not move between reads.
        modems.sort((a, b) -> a.interfaceId.compareTo(b.interfaceId));
        return new Snapshot(status[1], modems);
    }

    private static int number(String field) {
        if (field.equals("-")) return ABSENT;
        long value = Long.parseLong(field);
        if (value < 0 || value > 0xFFFFFFFFL) throw new IllegalArgumentException("range");
        return (int) value;
    }

    private static String text(String field) {
        if (field.equals("-")) return null;
        if ((field.length() & 1) != 0) throw new IllegalArgumentException("hex");
        byte[] bytes = new byte[field.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            int hi = Character.digit(field.charAt(2 * i), 16);
            int lo = Character.digit(field.charAt(2 * i + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("hex");
            bytes[i] = (byte) (hi << 4 | lo);
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static List<String> numbers(String field) {
        if (field.equals("-") || field.isEmpty()) return Collections.emptyList();
        List<String> result = new ArrayList<>();
        for (String hex : field.split(",", -1)) result.add(text(hex));
        return Collections.unmodifiableList(result);
    }

    // ---- Translation into Android values. ----

    /**
     * TelephonyManager's SIM state for the modem's ready state. SIM_STATE_LOADED is the
     * service-side value; TelephonyManager.getSimState reports it as SIM_STATE_READY.
     */
    static int simState(Modem modem) {
        if (modem == null) return TelephonyManager.SIM_STATE_UNKNOWN;
        switch (modem.readyState) {
            case READY_INITIALIZED: return TelephonyManager.SIM_STATE_LOADED;
            case READY_SIM_NOT_INSERTED:
            case READY_NO_ESIM_PROFILE: return TelephonyManager.SIM_STATE_ABSENT;
            case READY_BAD_SIM: return TelephonyManager.SIM_STATE_CARD_IO_ERROR;
            case READY_DEVICE_LOCKED: return TelephonyManager.SIM_STATE_PIN_REQUIRED;
            case READY_DEVICE_BLOCKED: return TelephonyManager.SIM_STATE_PUK_REQUIRED;
            case READY_OFF:
            case READY_NOT_ACTIVATED: return TelephonyManager.SIM_STATE_NOT_READY;
            default: return TelephonyManager.SIM_STATE_UNKNOWN;
        }
    }

    /** IccCardConstants.State names, as the gsm.sim.state property holds them. */
    static String simStateProperty(int simState) {
        switch (simState) {
            case TelephonyManager.SIM_STATE_ABSENT: return "ABSENT";
            case TelephonyManager.SIM_STATE_PIN_REQUIRED: return "PIN_REQUIRED";
            case TelephonyManager.SIM_STATE_PUK_REQUIRED: return "PUK_REQUIRED";
            case TelephonyManager.SIM_STATE_NOT_READY: return "NOT_READY";
            case TelephonyManager.SIM_STATE_CARD_IO_ERROR: return "CARD_IO_ERROR";
            case TelephonyManager.SIM_STATE_LOADED: return "LOADED";
            default: return "UNKNOWN";
        }
    }

    /** The radio access network type for an MBN data class (the highest bit present). */
    static int networkType(int dataClass) {
        if (dataClass == ABSENT || dataClass == 0) return TelephonyManager.NETWORK_TYPE_UNKNOWN;
        if ((dataClass & DATA_5G_SA) != 0) return TelephonyManager.NETWORK_TYPE_NR;
        // 5G NSA is an LTE anchor with an NR secondary cell; Android reports LTE and
        // signals NR only through TelephonyDisplayInfo, which is not reported here.
        if ((dataClass & (DATA_5G_NSA | DATA_LTE)) != 0) return TelephonyManager.NETWORK_TYPE_LTE;
        if ((dataClass & DATA_HSDPA) != 0 && (dataClass & DATA_HSUPA) != 0) {
            return TelephonyManager.NETWORK_TYPE_HSPA;
        }
        if ((dataClass & DATA_HSDPA) != 0) return TelephonyManager.NETWORK_TYPE_HSDPA;
        if ((dataClass & DATA_HSUPA) != 0) return TelephonyManager.NETWORK_TYPE_HSUPA;
        if ((dataClass & DATA_UMTS) != 0) return TelephonyManager.NETWORK_TYPE_UMTS;
        if ((dataClass & DATA_EDGE) != 0) return TelephonyManager.NETWORK_TYPE_EDGE;
        if ((dataClass & DATA_GPRS) != 0) return TelephonyManager.NETWORK_TYPE_GPRS;
        if ((dataClass & DATA_1XEVDO_REVB) != 0) return TelephonyManager.NETWORK_TYPE_EVDO_B;
        if ((dataClass & DATA_1XEVDO_REVA) != 0) return TelephonyManager.NETWORK_TYPE_EVDO_A;
        if ((dataClass & DATA_1XEVDO) != 0) return TelephonyManager.NETWORK_TYPE_EVDO_0;
        if ((dataClass & DATA_1XRTT) != 0) return TelephonyManager.NETWORK_TYPE_1xRTT;
        return TelephonyManager.NETWORK_TYPE_UNKNOWN;
    }

    /** The data network type only while registered; otherwise unknown. */
    static int dataNetworkType(Modem modem) {
        return modem != null && modem.registered() ? networkType(modem.currentDataClass)
                : TelephonyManager.NETWORK_TYPE_UNKNOWN;
    }

    /** TelephonyManager.DATA_* for the packet connection's activation state. */
    static int dataState(Modem modem) {
        if (modem == null) return TelephonyManager.DATA_DISCONNECTED;
        switch (modem.activation) {
            case ACTIVATION_ACTIVATED: return TelephonyManager.DATA_CONNECTED;
            case ACTIVATION_ACTIVATING: return TelephonyManager.DATA_CONNECTING;
            case ACTIVATION_DEACTIVATING: return TelephonyManager.DATA_DISCONNECTING;
            default: return TelephonyManager.DATA_DISCONNECTED;
        }
    }

    /** RSSI in dBm for a 27.007 CSQ value 0..31; UNAVAILABLE otherwise. */
    static int rssiDbm(int csq) {
        if (csq < 0 || csq > 31) return CellInfo.UNAVAILABLE;
        return -113 + 2 * csq;
    }

    /**
     * The modem's signal as Android's SignalStrength: Windows reports only RSSI (and a bit
     * error rate), so only those fields are set, in the measurement class of the current
     * radio access technology. NR standalone and CDMA have no RSSI field in Android's
     * classes and stay empty, as do unknown readings.
     */
    static SignalStrength signalStrength(Modem modem) {
        if (modem == null) return null;
        int rssi = rssiDbm(modem.signal);
        int ber = modem.signalError >= 0 && modem.signalError <= 7 ? modem.signalError
                : CellInfo.UNAVAILABLE;
        if (rssi == CellInfo.UNAVAILABLE || !modem.registered()) return new SignalStrength();
        int u = CellInfo.UNAVAILABLE;
        // SignalStrength takes every technology; unmeasured ones are empty (invalid).
        CellSignalStrengthGsm gsm = new CellSignalStrengthGsm();
        CellSignalStrengthWcdma wcdma = new CellSignalStrengthWcdma();
        CellSignalStrengthLte lte = new CellSignalStrengthLte();
        switch (networkType(modem.currentDataClass)) {
            case TelephonyManager.NETWORK_TYPE_LTE:
                lte = new CellSignalStrengthLte(rssi, u, u, u, u, u, u);
                break;
            case TelephonyManager.NETWORK_TYPE_UMTS:
            case TelephonyManager.NETWORK_TYPE_HSDPA:
            case TelephonyManager.NETWORK_TYPE_HSUPA:
            case TelephonyManager.NETWORK_TYPE_HSPA:
                wcdma = new CellSignalStrengthWcdma(rssi, ber, u, u);
                break;
            case TelephonyManager.NETWORK_TYPE_GPRS:
            case TelephonyManager.NETWORK_TYPE_EDGE:
                gsm = new CellSignalStrengthGsm(rssi, ber, u);
                break;
            default:
                return new SignalStrength();
        }
        return new SignalStrength(new android.telephony.CellSignalStrengthCdma(), gsm, wcdma,
                new android.telephony.CellSignalStrengthTdscdma(), lte,
                new android.telephony.CellSignalStrengthNr());
    }

    /** The registered network's MCC+MNC, when it is a 5 or 6 digit PLMN. */
    static String plmn(String id) {
        if (id == null || id.length() < 5 || id.length() > 6) return null;
        for (int i = 0; i < id.length(); i++) {
            if (id.charAt(i) < '0' || id.charAt(i) > '9') return null;
        }
        return id;
    }

    /** ServiceState of a data-only modem: no circuit-switched (voice) service. */
    static ServiceState serviceState(Modem modem) {
        return serviceState(modem, true);
    }

    /**
     * ServiceState, with the registered operator's names and numeric only when the
     * caller may see coarse location. This equals AOSP's createLocationInfoSanitizedCopy:
     * no cell identity is ever reported here, so only the operator fields differ.
     */
    static ServiceState serviceState(Modem modem, boolean operator) {
        ServiceState state = new ServiceState();
        if (modem == null || modem.readyState == READY_OFF) {
            state.setStateOff();
            return state;
        }
        state.setStateOutOfService();
        boolean registered = modem.registered();
        String plmn = registered ? plmn(modem.registeredProviderId) : null;
        int registration;
        if (registered) {
            registration = modem.roaming()
                    ? android.telephony.NetworkRegistrationInfo.REGISTRATION_STATE_ROAMING
                    : android.telephony.NetworkRegistrationInfo.REGISTRATION_STATE_HOME;
        } else if (modem.registerState == REGISTER_SEARCHING) {
            registration = android.telephony.NetworkRegistrationInfo
                    .REGISTRATION_STATE_NOT_REGISTERED_SEARCHING;
        } else if (modem.registerState == REGISTER_DENIED) {
            registration = android.telephony.NetworkRegistrationInfo.REGISTRATION_STATE_DENIED;
        } else {
            registration = android.telephony.NetworkRegistrationInfo
                    .REGISTRATION_STATE_NOT_REGISTERED_OR_SEARCHING;
        }
        android.telephony.NetworkRegistrationInfo.Builder data =
                new android.telephony.NetworkRegistrationInfo.Builder()
                        .setDomain(android.telephony.NetworkRegistrationInfo.DOMAIN_PS)
                        .setTransportType(android.telephony.AccessNetworkConstants.TRANSPORT_TYPE_WWAN)
                        .setRegistrationState(registration)
                        .setAccessNetworkTechnology(dataNetworkType(modem));
        if (plmn != null) data.setRegisteredPlmn(plmn);
        if (registered) {
            data.setAvailableServices(Collections.singletonList(
                    android.telephony.NetworkRegistrationInfo.SERVICE_TYPE_DATA));
        }
        state.addNetworkRegistrationInfo(data.build());
        state.setDataRegState(registered ? ServiceState.STATE_IN_SERVICE
                : ServiceState.STATE_OUT_OF_SERVICE);
        state.setDataRoaming(modem.roaming());
        if (registered && operator) {
            String name = modem.registeredProviderName;
            state.setOperatorName(name, name, plmn);
        }
        return state;
    }

    /** MCC table for country codes: AOSP MccTable's legacy entries (Apache 2.0). */
    private static final String MCC_ISO = ""
            + "202gr204nl206be208fr212mc213ad214es216hu218ba219hr220rs221xk222it225va226ro228ch"
            + "230cz231sk232at234gb235gb238dk240se242no244fi246lt247lv248ee250ru255ua257by259md"
            + "260pl262de266gi268pt270lu272ie274is276al278mt280cy282ge283am284bg286tr288fo289ge"
            + "290gl292sm293si294mk295li297me302ca308pm310us311us312us313us314us315us316us330pr"
            + "332vi334mx338jm340gp342bb344ag346ky348vg350bm352gd354ms356kn358lc360vc362cw363aw"
            + "364bs365ai366dm368cu370do372ht374tt376tc400az401kz402bt404in405in406in410pk412af"
            + "413lk414mm415lb416jo417sy418iq419kw420sa421ye422om423ps424ae425il426bh427qa428mn"
            + "429np430ae431ae432ir434uz436tj437kg438tm440jp441jp450kr452vn454hk455mo456kh457la"
            + "460cn461cn466tw467kp470bd472mv502my505au510id514tl515ph520th525sg528bn530nz536nr"
            + "537pg539to540sb541vu542fj543wf544as545ki546nc547pf548ck549ws550fm551mh552pw553tv"
            + "554tk555nu602eg603dz604ma605tn606ly607gm608sn609mr610ml611gn612ci613bf614ne615tg"
            + "616bj617mu618lr619sl620gh621ng622td623cf624cm625cv626st627gq628ga629cg630cd631ao"
            + "632gw633sc634sd635rw636et637so638dj639ke640tz641ug642bi643mz645zm646mg647re648zw"
            + "649na650mw651ls652bw653sz654km655za657er658sh659ss702bz704gt706sv708hn710ni712cr"
            + "714pa716pe722ar724br730cl732co734ve736bo738gy740ec742gf744py746sr748uy750fk";

    /** ISO 3166 country code for a PLMN's MCC, or "" when unknown. */
    static String countryIso(String plmn) {
        if (plmn == null || plmn.length() < 3) return "";
        String mcc = plmn.substring(0, 3);
        for (int i = 0; i + 5 <= MCC_ISO.length(); i += 5) {
            if (MCC_ISO.startsWith(mcc, i)) return MCC_ISO.substring(i + 3, i + 5);
        }
        return "";
    }

    /**
     * Android's phone type for a present modem: always GSM, as Android 16's
     * TelephonyManager.getPhoneType()/getCurrentPhoneType() answer (legacy CDMA is gone).
     */
    static int phoneType(Modem modem) {
        return modem == null ? TelephonyManager.PHONE_TYPE_NONE : TelephonyManager.PHONE_TYPE_GSM;
    }

    /**
     * Whether a SIM card is in the modem, as UiccProfile.hasIccCard answers: a card in
     * any state (ready, locked, blocked, failed or not activated); not for an empty slot,
     * a missing eSIM profile, a radio that is off or a failed reading.
     */
    static boolean cardPresent(Modem modem) {
        if (modem == null) return false;
        switch (modem.readyState) {
            case READY_INITIALIZED:
            case READY_BAD_SIM:
            case READY_DEVICE_LOCKED:
            case READY_DEVICE_BLOCKED:
            case READY_NOT_ACTIVATED:
                return true;
            default:
                return false;
        }
    }

    /**
     * TelephonyDisplayInfo for the data network: LTE carrying a 5G NSA secondary cell is
     * LTE overridden as NR_NSA, as DisplayInfoController reports it; otherwise no override.
     */
    static android.telephony.TelephonyDisplayInfo displayInfo(Modem modem) {
        int type = dataNetworkType(modem);
        boolean nsa = type == TelephonyManager.NETWORK_TYPE_LTE
                && (modem.currentDataClass & DATA_5G_NSA) != 0;
        return new android.telephony.TelephonyDisplayInfo(type, nsa
                ? android.telephony.TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NR_NSA
                : android.telephony.TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NONE,
                modem != null && modem.roaming());
    }

    /** The IMEI, for a GSM-class modem whose device ID is one (15 digits). */
    static String imei(Modem modem) {
        if (modem == null || modem.cellularClass != CLASS_GSM || modem.deviceId == null) return null;
        String id = modem.deviceId.trim();
        if (id.length() != 15) return null;
        for (int i = 0; i < id.length(); i++) {
            if (id.charAt(i) < '0' || id.charAt(i) > '9') return null;
        }
        return id;
    }

    /** ICCID with SIM padding 'F's removed, as SubscriptionManagerService stores it. */
    static String iccId(Modem modem) {
        if (modem == null || modem.iccId == null) return null;
        String id = modem.iccId.trim();
        int end = id.length();
        while (end > 0 && (id.charAt(end - 1) == 'F' || id.charAt(end - 1) == 'f')) end--;
        id = id.substring(0, end);
        return id.isEmpty() ? null : id;
    }

    /** The subscriber's first MSISDN as Windows reports it, or null. */
    static String number(Modem modem) {
        if (modem == null) return null;
        for (String n : modem.numbers) {
            if (n != null && !n.trim().isEmpty()) return n.trim();
        }
        return null;
    }
}
