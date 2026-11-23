/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.RemoteException;
import android.sysprop.TelephonyProperties;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import com.android.internal.telephony.IOnSubscriptionsChangedListener;
import com.android.internal.telephony.IPhoneStateListener;
import com.android.internal.telephony.IPhoneSubInfo;
import com.android.internal.telephony.ISub;
import com.android.internal.telephony.ITelephony;
import com.android.internal.telephony.ITelephonyRegistry;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/**
 * Android telephony backed by the host's cellular modem ({@link WindowsCellular}):
 * the {@code phone}, {@code iphonesubinfo}, {@code isub} and {@code telephony.registry}
 * services. See docs/telephony.md.
 *
 * <p>Gesher models at most one modem, Android phone 0, and only when Windows reported a
 * WWAN interface at boot. Without one the device has no FEATURE_TELEPHONY, and AOSP then
 * creates no PhoneGlobals: {@code phone}, {@code iphonesubinfo} and {@code isub} do not
 * exist, so TelephonyManager and SubscriptionManager answer from their "service is null"
 * paths without any permission check. Gesher's three services give exactly those answers
 * (see {@link #NO_TELEPHONY_SERVICE}); {@code telephony.registry} (system_server, present
 * on every device) keeps its checks. Data-only: there are no voice calls,
 * SMS, carrier privileges, carrier configuration or eUICC management. Identifier and phone
 * number access follow AOSP's TelephonyPermissions / LegacyPermissionManagerService rules.
 */
public final class TelephonyService {
    private TelephonyService() {
    }

    static final String READ_PHONE_STATE = "android.permission.READ_PHONE_STATE";
    static final String READ_BASIC_PHONE_STATE = "android.permission.READ_BASIC_PHONE_STATE";
    static final String READ_PRIVILEGED_PHONE_STATE = "android.permission.READ_PRIVILEGED_PHONE_STATE";
    static final String READ_PRECISE_PHONE_STATE = "android.permission.READ_PRECISE_PHONE_STATE";
    static final String READ_PHONE_NUMBERS = "android.permission.READ_PHONE_NUMBERS";
    static final String READ_SMS = "android.permission.READ_SMS";
    static final String USE_ICC_AUTH = "android.permission.USE_ICC_AUTH_WITH_DEVICE_IDENTIFIER";
    static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    static final String READ_ACTIVE_EMERGENCY_SESSION = "android.permission.READ_ACTIVE_EMERGENCY_SESSION";

    /**
     * Without FEATURE_TELEPHONY, AOSP's PhoneGlobals.onCreate returns before publishing
     * {@code phone}, {@code iphonesubinfo} or {@code isub} (config_force_phone_globals_creation
     * is false). Gesher still registers them, so without a boot modem each method answers as
     * the manager does when the service is null: the same value, the same
     * IllegalStateException where the manager throws one, and no permission check.
     */
    static final String NO_TELEPHONY_SERVICE = "Telephony service not available.";
    static final String NO_SUBSCRIPTION_SERVICE = "subscription service unavailable.";

    // ---------------------------------------------------------------------------------
    // The modem's state, polled by one daemon worker, and the boot probe for features.
    // ---------------------------------------------------------------------------------

    private static final long BOOT_PROBE_SECONDS = 5;
    private static volatile WindowsCellular.Snapshot sBoot;
    private static volatile boolean sBootProbed;

    /**
     * The snapshot taken once at boot, from which system features and the phone count
     * are fixed (as Android's are for the life of a boot). A probe that does not finish in
     * a few seconds, or fails, counts as no modem.
     */
    static synchronized WindowsCellular.Snapshot bootSnapshot(WindowsCellular.Source source) {
        if (sBootProbed) return sBoot;
        sBootProbed = true;
        AtomicReference<String> raw = new AtomicReference<>();
        Thread probe = new Thread(() -> raw.set(source.snapshot()), "gesher-cellular-probe");
        probe.setDaemon(true);
        probe.start();
        try {
            probe.join(TimeUnit.SECONDS.toMillis(BOOT_PROBE_SECONDS));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        WindowsCellular.Snapshot snapshot = WindowsCellular.parse(raw.get());
        sBoot = snapshot != null && "ok".equals(snapshot.status) ? snapshot : null;
        return sBoot;
    }

    /** The production boot snapshot of Windows' Mobile Broadband service. */
    public static WindowsCellular.Snapshot bootSnapshot() {
        return bootSnapshot(WindowsCellular.WINDOWS);
    }

    /** Receives the Android telephony properties derived from the modem. */
    interface PropertySink {
        void publish(WindowsCellular.Modem modem, int simState);
    }

    /** Publishes gsm.* properties as the phone process does on a device (TelephonyProperties). */
    static final PropertySink SYSTEM_PROPERTIES = (modem, simState) -> {
        boolean sim = modem != null && modem.simReady();
        String home = sim ? WindowsCellular.plmn(modem.homeProviderId) : null;
        boolean registered = modem != null && modem.registered();
        String network = registered ? WindowsCellular.plmn(modem.registeredProviderId) : null;
        TelephonyProperties.sim_state(List.of(WindowsCellular.simStateProperty(simState)));
        TelephonyProperties.icc_operator_numeric(List.of(home == null ? "" : home));
        TelephonyProperties.icc_operator_alpha(List.of(sim && modem.homeProviderName != null
                ? modem.homeProviderName : ""));
        TelephonyProperties.icc_operator_iso_country(List.of(WindowsCellular.countryIso(home)));
        TelephonyProperties.operator_numeric(List.of(network == null ? "" : network));
        TelephonyProperties.operator_alpha(List.of(registered && modem.registeredProviderName != null
                ? modem.registeredProviderName : ""));
        TelephonyProperties.operator_iso_country(List.of(WindowsCellular.countryIso(network)));
        TelephonyProperties.operator_is_roaming(List.of(modem != null && modem.roaming()));
        TelephonyProperties.data_network_type(List.of(TelephonyManager.getNetworkTypeName(
                WindowsCellular.dataNetworkType(modem))));
        TelephonyProperties.current_active_phone(List.of(WindowsCellular.phoneType(modem)));
        TelephonyProperties.baseband_version(List.of(modem != null && modem.firmware != null
                ? modem.firmware : ""));
    };

    /** The modem behind Android phone 0, if one was present at boot. */
    static final class Cellular {
        private static final long POLL_MILLIS = 5000;
        private static final int MAX_FAILED_POLLS = 3;
        private final WindowsCellular.Source mSource;
        private final PropertySink mProperties;
        private final boolean mPresent;
        private final String mInterfaceId;
        private final Object mLock = new Object();
        private final List<Runnable> mListeners = new CopyOnWriteArrayList<>();
        // Subscription IDs are assigned per ICCID for this boot, from 1 as Android's
        // subscription database assigns them; they are not persisted across restarts.
        private final Map<String, Integer> mSubIds = new HashMap<>();
        private int mNextSubId = 1;
        private WindowsCellular.Modem mModem;
        private int mSubId = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
        private int mFailures;
        private Thread mWorker;

        Cellular(WindowsCellular.Source source, WindowsCellular.Snapshot boot, PropertySink properties) {
            mSource = Objects.requireNonNull(source);
            mProperties = Objects.requireNonNull(properties);
            WindowsCellular.Modem modem = boot == null ? null : boot.primary();
            mPresent = modem != null;
            mInterfaceId = mPresent ? modem.interfaceId : null;
            if (mPresent) {
                synchronized (mLock) {
                    mModem = modem;
                    mSubId = subscriptionFor(modem);
                }
                publish(modem);
            }
        }

        /** Whether phone 0 exists (fixed at boot). */
        boolean present() { return mPresent; }

        /** The latest reading of the boot modem; null if it has gone or cannot be read. */
        WindowsCellular.Modem modem() {
            synchronized (mLock) { return mModem; }
        }

        /** The active subscription's ID, or INVALID_SUBSCRIPTION_ID without a ready SIM. */
        int subId() {
            synchronized (mLock) { return mSubId; }
        }

        int simState() {
            if (!mPresent) return TelephonyManager.SIM_STATE_UNKNOWN;
            WindowsCellular.Modem modem = modem();
            return modem == null ? TelephonyManager.SIM_STATE_NOT_READY : WindowsCellular.simState(modem);
        }

        void addListener(Runnable listener) { mListeners.add(listener); }

        /** Starts the polling worker; nothing runs without a modem. */
        void start() {
            if (!mPresent) return;
            synchronized (mLock) {
                if (mWorker != null) return;
                mWorker = new Thread(this::poll, "gesher-cellular");
                mWorker.setDaemon(true);
                mWorker.start();
            }
        }

        private void poll() {
            while (true) {
                try {
                    Thread.sleep(POLL_MILLIS);
                } catch (InterruptedException e) {
                    return;
                }
                refresh();
            }
        }

        /** One read; listeners hear about changes. Returns whether anything changed. */
        boolean refresh() {
            if (!mPresent) return false;
            WindowsCellular.Snapshot snapshot = WindowsCellular.parse(mSource.snapshot());
            WindowsCellular.Modem next = null;
            boolean readable = snapshot != null && !"error".equals(snapshot.status);
            if (readable) {
                for (WindowsCellular.Modem modem : snapshot.modems) {
                    if (modem.interfaceId.equals(mInterfaceId)) next = modem;
                }
            }
            synchronized (mLock) {
                if (!readable) {
                    // A transient read failure keeps the last reading briefly; it is not
                    // presented as current for longer than a few polls.
                    if (++mFailures < MAX_FAILED_POLLS) return false;
                } else {
                    mFailures = 0;
                }
                if (Objects.equals(mModem == null ? null : mModem.source,
                        next == null ? null : next.source)) {
                    return false;
                }
                mModem = next;
                mSubId = subscriptionFor(next);
            }
            publish(next);
            for (Runnable listener : mListeners) {
                try {
                    listener.run();
                } catch (RuntimeException e) {
                    // One observer's failure does not stop others.
                }
            }
            return true;
        }

        // Called with mLock or from the constructor.
        private int subscriptionFor(WindowsCellular.Modem modem) {
            String iccId = WindowsCellular.iccId(modem);
            if (modem == null || !modem.simReady() || iccId == null) {
                return SubscriptionManager.INVALID_SUBSCRIPTION_ID;
            }
            Integer id = mSubIds.get(iccId);
            if (id == null) {
                id = mNextSubId++;
                mSubIds.put(iccId, id);
            }
            return id;
        }

        private void publish(WindowsCellular.Modem modem) {
            try {
                mProperties.publish(modem, modem == null ? TelephonyManager.SIM_STATE_NOT_READY
                        : WindowsCellular.simState(modem));
            } catch (RuntimeException e) {
                // Properties are a convenience mirror; the services answer from the model.
            }
        }

        /** Android phone index for a subscription ID: 0 for the active one, else invalid. */
        int phoneForSub(int subId) {
            int active = subId();
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = active;
            return mPresent && SubscriptionManager.isValidSubscriptionId(subId) && subId == active
                    ? 0 : SubscriptionManager.INVALID_PHONE_INDEX;
        }

        boolean phone(int phoneId) {
            return mPresent && phoneId == 0;
        }
    }

    // ---------------------------------------------------------------------------------
    // Callers and Android's telephony permission rules.
    // ---------------------------------------------------------------------------------

    /** An authenticated caller. System callers are Gesher's own services and shell. */
    record Caller(String packageName, String attributionTag, int uid, int targetSdk, boolean system) {
    }

    /**
     * The permission primitives the rules below are built on: Binder authentication,
     * granted permissions, app-op notes and the location switch. Production uses the
     * package manager and AppOps; host fixtures substitute this boundary only.
     */
    interface Callers {
        /** The calling package, authenticated; the claim null means "whatever the caller is". */
        Caller authenticate(String claimedPackage, String attributionTag);
        boolean granted(Caller caller, String permission);
        /** AppOpsManager MODE_* after noting the operation. */
        int noteOp(Caller caller, String op);
        boolean locationEnabled();
    }

    static Callers callers(PackageManagerService pm, ActivityManagerService am,
            BooleanSupplier locationEnabled) {
        return new Callers() {
            @Override
            public Caller authenticate(String claimed, String tag) {
                CallerIdentity identity = am.callers();
                int uid = Binder.getCallingUid();
                if (identity.isSystem()) {
                    return new Caller(claimed == null ? "android" : claimed, tag, uid,
                            Build.VERSION.SDK_INT, true);
                }
                String name = claimed != null ? claimed : identity.callerPackage();
                ApkPackage code = identity.requireCurrentPackage(name);
                if (code == null) throw new SecurityException("Unknown package " + name);
                return new Caller(code.packageName, tag, uid,
                        code.applicationInfo(0).targetSdkVersion, false);
            }

            @Override
            public boolean granted(Caller caller, String permission) {
                return caller.system() || pm.permissions().checkPermission(caller.packageName(),
                        permission) == PackageManager.PERMISSION_GRANTED;
            }

            @Override
            public int noteOp(Caller caller, String op) {
                if (caller.system()) return AppOpsManager.MODE_ALLOWED;
                long token = Binder.clearCallingIdentity();
                try {
                    return pm.appOps().noteOperation(AppOpsManager.strOpToOp(op), caller.uid(),
                            caller.packageName(), caller.attributionTag(), false, null, false)
                            .getOpMode();
                } finally {
                    Binder.restoreCallingIdentity(token);
                }
            }

            @Override
            public boolean locationEnabled() {
                return locationEnabled.getAsBoolean();
            }
        };
    }

    /** AOSP TelephonyPermissions, without carrier privileges (no carrier apps exist). */
    static final class Rules {
        private final Callers mCallers;

        Rules(Callers callers) {
            mCallers = Objects.requireNonNull(callers);
        }

        Caller caller(String claimedPackage, String tag) {
            return mCallers.authenticate(claimedPackage, tag);
        }

        boolean granted(Caller c, String permission) { return mCallers.granted(c, permission); }

        /** checkReadPhoneState: true, false (app op revoked) or SecurityException. */
        boolean readPhoneState(Caller c, String message) {
            if (granted(c, READ_PRIVILEGED_PHONE_STATE)) return true;
            if (!granted(c, READ_PHONE_STATE)) {
                throw new SecurityException(message + ": Neither user " + c.uid()
                        + " nor current process has " + READ_PHONE_STATE + ".");
            }
            return mCallers.noteOp(c, AppOpsManager.OPSTR_READ_PHONE_STATE) == AppOpsManager.MODE_ALLOWED;
        }

        boolean readPhoneStateNoThrow(Caller c, String message) {
            try {
                return readPhoneState(c, message);
            } catch (SecurityException e) {
                return false;
            }
        }

        boolean readNonDangerousPhoneState(Caller c) {
            return granted(c, READ_BASIC_PHONE_STATE);
        }

        /** Device identifiers (IMEI) and subscriber identifiers (IMSI, ICCID). */
        boolean readIdentifiers(Caller c, String message) {
            int icc = mCallers.noteOp(c, AppOpsManager.OPSTR_USE_ICC_AUTH_WITH_DEVICE_IDENTIFIER);
            if (icc == AppOpsManager.MODE_ALLOWED
                    || icc == AppOpsManager.MODE_DEFAULT && granted(c, USE_ICC_AUTH)) {
                return true;
            }
            if (c.system() || granted(c, READ_PRIVILEGED_PHONE_STATE)) return true;
            if (mCallers.noteOp(c, AppOpsManager.OPSTR_READ_DEVICE_IDENTIFIERS)
                    == AppOpsManager.MODE_ALLOWED) {
                return true;
            }
            // Apps targeting pre-Q with READ_PHONE_STATE get placeholder (null) data.
            if (c.targetSdk() < Build.VERSION_CODES.Q && granted(c, READ_PHONE_STATE)) return false;
            throw new SecurityException(message + ": The uid " + c.uid()
                    + " does not meet the requirements to access device identifiers.");
        }

        boolean readIdentifiersNoThrow(Caller c, String message) {
            try {
                return readIdentifiers(c, message);
            } catch (SecurityException e) {
                return false;
            }
        }

        private int permissionAndOp(Caller c, String permission, String op) {
            if (permission != null && !granted(c, permission)) return PackageManager.PERMISSION_DENIED;
            return mCallers.noteOp(c, op) == AppOpsManager.MODE_ALLOWED
                    ? PackageManager.PERMISSION_GRANTED : AppOpsManager.MODE_IGNORED;
        }

        /** checkReadPhoneNumber: true, false (app op denied) or SecurityException. */
        boolean readPhoneNumber(Caller c, String message) {
            if (granted(c, READ_PRIVILEGED_PHONE_STATE)) return true;
            int result = PackageManager.PERMISSION_DENIED;
            if (c.targetSdk() <= Build.VERSION_CODES.Q) {
                result = permissionAndOp(c, READ_PHONE_STATE, AppOpsManager.OPSTR_READ_PHONE_STATE);
                if (result == PackageManager.PERMISSION_GRANTED) return true;
            }
            // The default SMS app (WRITE_SMS app op), READ_PHONE_NUMBERS or READ_SMS.
            if (permissionAndOp(c, null, AppOpsManager.OPSTR_WRITE_SMS) == PackageManager.PERMISSION_GRANTED
                    || permissionAndOp(c, READ_PHONE_NUMBERS, AppOpsManager.OPSTR_READ_PHONE_NUMBERS)
                            == PackageManager.PERMISSION_GRANTED
                    || permissionAndOp(c, READ_SMS, AppOpsManager.OPSTR_READ_SMS)
                            == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
            if (result == AppOpsManager.MODE_IGNORED) return false;
            throw new SecurityException(message + ": Neither user " + c.uid()
                    + " nor current process has " + READ_PHONE_STATE + ", " + READ_SMS + ", or "
                    + READ_PHONE_NUMBERS);
        }

        boolean readPhoneNumberNoThrow(Caller c, String message) {
            try {
                return readPhoneNumber(c, message);
            } catch (SecurityException e) {
                return false;
            }
        }

        /**
         * LocationAccessPolicy for telephony: the permission, its app op and the location
         * switch must all allow access. Pre-Q coarse-only apps are not given fine access.
         */
        boolean fineLocation(Caller c) {
            if (c.system()) return true;
            return granted(c, FINE) && mCallers.noteOp(c, AppOpsManager.OPSTR_FINE_LOCATION)
                    == AppOpsManager.MODE_ALLOWED && mCallers.locationEnabled();
        }

        boolean coarseLocation(Caller c) {
            if (c.system()) return true;
            if (fineLocation(c)) return true;
            return granted(c, COARSE) && mCallers.noteOp(c, AppOpsManager.OPSTR_COARSE_LOCATION)
                    == AppOpsManager.MODE_ALLOWED && mCallers.locationEnabled();
        }

        boolean anyLocationPermission(Caller c) {
            return c.system() || granted(c, FINE) || granted(c, COARSE);
        }
    }

    /**
     * ServiceState as a caller may see it: the registered operator is location data
     * (AOSP sanitizes it without coarse location). Fine location adds nothing here,
     * since no cell identity is reported.
     */
    static ServiceState forCaller(WindowsCellular.Modem modem, boolean fine, boolean coarse) {
        return WindowsCellular.serviceState(modem, fine || coarse);
    }

    // ---------------------------------------------------------------------------------
    // phone: ITelephony.
    // ---------------------------------------------------------------------------------

    static final class Phone extends ITelephony.Default {
        private final Cellular mCellular;
        private final Rules mRules;

        Phone(Cellular cellular, Rules rules) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
        }

        private WindowsCellular.Modem modemForSub(int subId) {
            return mCellular.phoneForSub(subId) == 0 ? mCellular.modem() : null;
        }

        /**
         * The SIM state in a slot: UNKNOWN for a slot without a phone, as
         * PhoneInterfaceManager answers and as TelephonyManager.getSimState(int) answers when
         * the service is null. Without a modem there are no phones, so
         * TelephonyManager.getSimState() is ABSENT without asking this service.
         */
        @Override
        public int getSimStateForSlotIndex(int slotIndex) {
            if (slotIndex != 0 || !mCellular.present()) return TelephonyManager.SIM_STATE_UNKNOWN;
            return mCellular.simState();
        }

        /** The registered network's country; "" without a phone or registration. */
        @Override
        public String getNetworkCountryIsoForPhone(int phoneId) {
            if (phoneId == SubscriptionManager.INVALID_PHONE_INDEX) phoneId = 0;
            if (!mCellular.phone(phoneId)) return "";
            WindowsCellular.Modem modem = mCellular.modem();
            if (modem == null || !modem.registered()) return "";
            return WindowsCellular.countryIso(WindowsCellular.plmn(modem.registeredProviderId));
        }

        @Override
        public int getDataNetworkTypeForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readNonDangerousPhoneState(c)
                    && !mRules.readPhoneState(c, "getDataNetworkTypeForSubscriber")) {
                return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            }
            return WindowsCellular.dataNetworkType(modemForSub(subId));
        }

        @Override
        public int getDataNetworkType(String callingPackage, String callingFeatureId) {
            return getDataNetworkTypeForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public int getNetworkTypeForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (c.targetSdk() > Build.VERSION_CODES.Q) {
                return getDataNetworkTypeForSubscriber(subId, callingPackage, callingFeatureId);
            }
            if (c.targetSdk() == Build.VERSION_CODES.Q
                    && !mRules.readPhoneStateNoThrow(c, "getNetworkTypeForSubscriber")) {
                return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            }
            return WindowsCellular.dataNetworkType(modemForSub(subId));
        }

        /** A data-only modem has no circuit-switched (voice) registration. */
        @Override
        public int getVoiceNetworkTypeForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "getVoiceNetworkTypeForSubscriber")) {
                return TelephonyManager.NETWORK_TYPE_UNKNOWN;
            }
            return TelephonyManager.NETWORK_TYPE_UNKNOWN;
        }

        @Override
        public int getDataStateForSubId(int subId) {
            return WindowsCellular.dataState(modemForSub(subId));
        }

        @Override
        public int getDataState() {
            return getDataStateForSubId(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID);
        }

        @Override
        public SignalStrength getSignalStrength(int subId) {
            if (mCellular.phoneForSub(subId) != 0) return null;
            WindowsCellular.Modem modem = mCellular.modem();
            return modem == null ? new SignalStrength() : WindowsCellular.signalStrength(modem);
        }

        @Override
        public ServiceState getServiceStateForSlot(int slotIndex, boolean renounceFine,
                boolean renounceCoarse, String callingPackage, String callingFeatureId) {
            if (!mCellular.phone(slotIndex)) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "getServiceStateForSubscriber")) return null;
            boolean fine = !renounceFine && mRules.fineLocation(c);
            boolean coarse = !renounceCoarse && mRules.coarseLocation(c);
            return forCaller(mCellular.modem(), fine, coarse);
        }

        @Override
        public boolean isRadioOnWithFeature(String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneStateNoThrow(c, "isRadioOnWithFeature")) return false;
            WindowsCellular.Modem modem = mCellular.phone(0) ? mCellular.modem() : null;
            return modem != null && modem.readyState != WindowsCellular.READY_OFF;
        }

        @Override
        public String getImeiForSlot(int slotIndex, String callingPackage, String callingFeatureId) {
            if (!mCellular.phone(slotIndex)) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getImeiForSlot")) return null;
            return WindowsCellular.imei(mCellular.modem());
        }

        @Override
        public String getPrimaryImei(String callingPackage, String callingFeatureId) {
            // TelephonyManager.getPrimaryImei throws this when the service is null.
            if (!mCellular.present()) throw new IllegalStateException(NO_TELEPHONY_SERVICE);
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiersNoThrow(c, "getPrimaryImei")) {
                throw new SecurityException("Caller does not have permission");
            }
            String imei = mCellular.phone(0) ? WindowsCellular.imei(mCellular.modem()) : null;
            if (imei == null) throw new UnsupportedOperationException("Operation not supported");
            return imei;
        }

        @Override
        public String getDeviceIdWithFeature(String callingPackage, String callingFeatureId) {
            if (!mCellular.phone(0)) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getDeviceId")) return null;
            return WindowsCellular.imei(mCellular.modem());
        }

        @Override
        public String getTypeAllocationCodeForSlot(int slotIndex) {
            String imei = mCellular.phone(slotIndex) ? WindowsCellular.imei(mCellular.modem()) : null;
            return imei == null ? null : imei.substring(0, 8);
        }

        /** IMEISV is not reported by Windows' Mobile Broadband API. */
        @Override
        public String getDeviceSoftwareVersionForSlot(int slotIndex, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.phone(slotIndex)) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            mRules.readPhoneState(c, "getDeviceSoftwareVersionForSlot");
            return null;
        }

        /** No carrier-set display number exists; the SIM's number comes from iphonesubinfo. */
        @Override
        public String getLine1NumberForDisplay(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            mRules.readPhoneNumber(c, "getLine1NumberForDisplay");
            return null;
        }

        /** No carrier identification database: Android's UNKNOWN_CARRIER_ID. */
        @Override
        public int getSubscriptionCarrierId(int subId) {
            return TelephonyManager.UNKNOWN_CARRIER_ID;
        }

        @Override
        public int getSubscriptionSpecificCarrierId(int subId) {
            return TelephonyManager.UNKNOWN_CARRIER_ID;
        }

        @Override
        public int getCarrierIdFromMccMnc(int slotIndex, String mccmnc, boolean isSubscriptionMccMnc) {
            return TelephonyManager.UNKNOWN_CARRIER_ID;
        }

        @Override
        public int getCarrierPrivilegeStatus(int subId) {
            return TelephonyManager.CARRIER_PRIVILEGE_STATUS_NO_ACCESS;
        }
    }

    // ---------------------------------------------------------------------------------
    // iphonesubinfo: IPhoneSubInfo.
    // ---------------------------------------------------------------------------------

    static final class PhoneSubInfo extends IPhoneSubInfo.Default {
        private final Cellular mCellular;
        private final Rules mRules;

        PhoneSubInfo(Cellular cellular, Rules rules) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
        }

        private WindowsCellular.Modem modemForSub(int subId) {
            return mCellular.phoneForSub(subId) == 0 ? mCellular.modem() : null;
        }

        @Override
        public String getDeviceIdForPhone(int phoneId, String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getDeviceId")) return null;
            return mCellular.phone(phoneId) ? WindowsCellular.imei(mCellular.modem()) : null;
        }

        @Override
        public String getDeviceIdWithFeature(String callingPackage, String callingFeatureId) {
            return getDeviceIdForPhone(mCellular.phoneForSub(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID),
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getImeiForSubscriber(int subId, String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getImei")) return null;
            return WindowsCellular.imei(modemForSub(subId));
        }

        @Override
        public String getSubscriberIdForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getSubscriberIdForSubscriber")) return null;
            WindowsCellular.Modem modem = modemForSub(subId);
            return modem == null || modem.subscriberId == null || modem.subscriberId.isEmpty()
                    ? null : modem.subscriberId;
        }

        @Override
        public String getIccSerialNumberForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getIccSerialNumber")) return null;
            return WindowsCellular.iccId(modemForSub(subId));
        }

        @Override
        public String getIccSerialNumberWithFeature(String callingPackage, String callingFeatureId) {
            return getIccSerialNumberForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getLine1NumberForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneNumber(c, "getLine1Number")) return null;
            return WindowsCellular.number(modemForSub(subId));
        }

        @Override
        public String getLine1Number(String callingPackage, String callingFeatureId) {
            return getLine1NumberForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }
    }

    // ---------------------------------------------------------------------------------
    // isub: ISub.
    // ---------------------------------------------------------------------------------

    static final class Subscriptions extends ISub.Default {
        private final Cellular mCellular;
        private final Rules mRules;

        Subscriptions(Cellular cellular, Rules rules) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
        }

        /** The active subscription's full record, or null. */
        SubscriptionInfo info() {
            int subId = mCellular.subId();
            WindowsCellular.Modem modem = mCellular.modem();
            if (!SubscriptionManager.isValidSubscriptionId(subId) || modem == null) return null;
            String home = WindowsCellular.plmn(modem.homeProviderId);
            String spn = modem.homeProviderName == null ? "" : modem.homeProviderName;
            String carrier = modem.registered() && modem.registeredProviderName != null
                    ? modem.registeredProviderName : spn;
            String iccId = WindowsCellular.iccId(modem);
            SubscriptionInfo.Builder builder = new SubscriptionInfo.Builder()
                    .setId(subId)
                    .setIccId(iccId)
                    .setCardString(iccId)
                    .setSimSlotIndex(0)
                    .setDisplayName(spn)
                    .setDisplayNameSource(spn.isEmpty() ? SubscriptionManager.NAME_SOURCE_UNKNOWN
                            : SubscriptionManager.NAME_SOURCE_SIM_SPN)
                    .setCarrierName(carrier)
                    .setNumber(WindowsCellular.number(modem))
                    .setCountryIso(WindowsCellular.countryIso(home))
                    .setCarrierId(TelephonyManager.UNKNOWN_CARRIER_ID)
                    .setEmbedded(false)
                    .setType(SubscriptionManager.SUBSCRIPTION_TYPE_LOCAL_SIM)
                    .setPortIndex(0)
                    .setUiccApplicationsEnabled(true);
            if (home != null) builder.setMcc(home.substring(0, 3)).setMnc(home.substring(3));
            return builder.build();
        }

        /** conditionallyRemoveIdentifiers. */
        private SubscriptionInfo redact(SubscriptionInfo info, Caller c, String message) {
            boolean identifiers = mRules.readIdentifiersNoThrow(c, message);
            boolean number = mRules.readPhoneNumberNoThrow(c, message);
            if (identifiers && number) return info;
            SubscriptionInfo.Builder result = new SubscriptionInfo.Builder(info);
            if (!identifiers) {
                result.setIccId(null);
                result.setCardString(null);
                result.setGroupUuid(null);
            }
            if (!number) result.setNumber(null);
            return result.build();
        }

        private boolean readPhoneStateOnAnySub(Caller c, String message) {
            if (mRules.granted(c, READ_PRIVILEGED_PHONE_STATE)) return true;
            if (!mRules.granted(c, READ_PHONE_STATE)) return false;
            return mRules.readPhoneStateNoThrow(c, message);
        }

        @Override
        public List<SubscriptionInfo> getActiveSubscriptionInfoList(String callingPackage,
                String callingFeatureId, boolean isForAllProfiles) {
            if (!mCellular.present()) return Collections.emptyList();
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!readPhoneStateOnAnySub(c, "getActiveSubscriptionInfoList")) {
                return Collections.emptyList();
            }
            SubscriptionInfo info = info();
            return info == null ? Collections.emptyList()
                    : List.of(redact(info, c, "getActiveSubscriptionInfoList"));
        }

        @Override
        public int getActiveSubInfoCount(String callingPackage, String callingFeatureId,
                boolean isForAllProfiles) {
            if (!mCellular.present()) return 0;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!readPhoneStateOnAnySub(c, "getAllSubInfoList")) {
                throw new SecurityException("Need READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE, or "
                        + "carrier privilege");
            }
            return info() == null ? 0 : 1;
        }

        @Override
        public int getActiveSubInfoCountMax() {
            return mCellular.present() ? 1 : 0;
        }

        @Override
        public SubscriptionInfo getActiveSubscriptionInfo(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "getActiveSubscriptionInfo")) {
                throw new SecurityException("Need READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE, or "
                        + "carrier privilege");
            }
            SubscriptionInfo info = info();
            return info != null && info.getSubscriptionId() == subId
                    ? redact(info, c, "getActiveSubscriptionInfo") : null;
        }

        @Override
        public SubscriptionInfo getActiveSubscriptionInfoForSimSlotIndex(int slotIndex,
                String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "getActiveSubscriptionInfoForSimSlotIndex")) {
                throw new SecurityException("Need READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE, or "
                        + "carrier privilege");
            }
            if (!SubscriptionManager.isValidSlotIndex(slotIndex)) {
                throw new IllegalArgumentException("Invalid slot index " + slotIndex);
            }
            SubscriptionInfo info = slotIndex == 0 ? info() : null;
            return info == null ? null : redact(info, c, "getActiveSubscriptionInfoForSimSlotIndex");
        }

        private List<SubscriptionInfo> privileged(String callingPackage, String callingFeatureId,
                String message) {
            if (!mCellular.present()) return Collections.emptyList();
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.granted(c, READ_PRIVILEGED_PHONE_STATE)) {
                throw new SecurityException(message + " requires " + READ_PRIVILEGED_PHONE_STATE);
            }
            SubscriptionInfo info = info();
            return info == null ? Collections.emptyList() : List.of(info);
        }

        @Override
        public List<SubscriptionInfo> getAllSubInfoList(String callingPackage, String callingFeatureId) {
            return privileged(callingPackage, callingFeatureId, "getAllSubInfoList");
        }

        @Override
        public List<SubscriptionInfo> getAvailableSubscriptionInfoList(String callingPackage,
                String callingFeatureId) {
            return privileged(callingPackage, callingFeatureId, "getAvailableSubscriptionInfoList");
        }

        @Override
        public int getDefaultSubId() { return mCellular.subId(); }
        @Override
        public int getDefaultSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override
        public int getDefaultDataSubId() { return getDefaultSubId(); }
        @Override
        public int getDefaultVoiceSubId() { return getDefaultSubId(); }
        @Override
        public int getDefaultVoiceSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override
        public int getDefaultSmsSubId() { return getDefaultSubId(); }
        @Override
        public int getDefaultSmsSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override
        public int getActiveDataSubscriptionId() { return getDefaultSubId(); }
        @Override
        public int getPreferredDataSubscriptionId() { return getDefaultDataSubId(); }

        @Override
        public int getSubId(int slotIndex) {
            if (slotIndex == SubscriptionManager.DEFAULT_SIM_SLOT_INDEX) slotIndex = 0;
            return slotIndex == 0 ? mCellular.subId() : SubscriptionManager.INVALID_SUBSCRIPTION_ID;
        }

        @Override
        public int getSlotIndex(int subId) {
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = getDefaultSubId();
            return SubscriptionManager.isValidSubscriptionId(subId) && subId == mCellular.subId()
                    ? 0 : SubscriptionManager.INVALID_SIM_SLOT_INDEX;
        }

        @Override
        public int getPhoneId(int subId) {
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = getDefaultSubId();
            if (!SubscriptionManager.isValidSubscriptionId(subId)) {
                return SubscriptionManager.INVALID_PHONE_INDEX;
            }
            // AOSP retains the default-phone sentinel for a valid but unmapped ID.
            return subId == mCellular.subId() ? 0 : SubscriptionManager.DEFAULT_PHONE_INDEX;
        }

        @Override
        public int getEnabledSubscriptionId(int slotIndex) {
            if (!SubscriptionManager.isValidSlotIndex(slotIndex)) {
                throw new IllegalArgumentException("Invalid slot index " + slotIndex);
            }
            return getSubId(slotIndex);
        }

        @Override
        public int[] getActiveSubIdList(boolean visibleOnly) {
            int subId = mCellular.subId();
            return SubscriptionManager.isValidSubscriptionId(subId) ? new int[] {subId} : new int[0];
        }

        @Override
        public boolean isActiveSubId(int subId, String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "isActiveSubId")) {
                throw new SecurityException("Need READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE, or "
                        + "carrier privilege");
            }
            return SubscriptionManager.isValidSubscriptionId(subId) && subId == mCellular.subId();
        }

        private String phoneNumber(int subId, String callingPackage, String callingFeatureId,
                String message) {
            // SubscriptionManager.getPhoneNumber throws this when the service is null.
            if (!mCellular.present()) throw new IllegalStateException(NO_SUBSCRIPTION_SERVICE);
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.granted(c, READ_PHONE_NUMBERS) && !mRules.granted(c, READ_PRIVILEGED_PHONE_STATE)) {
                throw new SecurityException(message + ": uid " + c.uid() + " has neither "
                        + READ_PHONE_NUMBERS + " nor " + READ_PRIVILEGED_PHONE_STATE);
            }
            if (subId == SubscriptionManager.INVALID_SUBSCRIPTION_ID) return "";
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = getDefaultSubId();
            else if (!SubscriptionManager.isValidSubscriptionId(subId)) {
                throw new IllegalArgumentException("Invalid SubId=" + subId);
            }
            if (!SubscriptionManager.isValidSubscriptionId(subId) || subId != mCellular.subId()) {
                return "";
            }
            String number = WindowsCellular.number(mCellular.modem());
            return number == null ? "" : number;
        }

        /** Only the SIM (UICC) source exists; carrier and IMS numbers are empty. */
        @Override
        public String getPhoneNumber(int subId, int source, String callingPackage,
                String callingFeatureId) {
            String number = phoneNumber(subId, callingPackage, callingFeatureId, "getPhoneNumber");
            if (source != SubscriptionManager.PHONE_NUMBER_SOURCE_UICC
                    && source != SubscriptionManager.PHONE_NUMBER_SOURCE_CARRIER
                    && source != SubscriptionManager.PHONE_NUMBER_SOURCE_IMS) {
                throw new IllegalArgumentException("Invalid number source " + source);
            }
            return source == SubscriptionManager.PHONE_NUMBER_SOURCE_UICC ? number : "";
        }

        @Override
        public String getPhoneNumberFromFirstAvailableSource(int subId, String callingPackage,
                String callingFeatureId) {
            return phoneNumber(subId, callingPackage, callingFeatureId,
                    "getPhoneNumberFromFirstAvailableSource");
        }
    }

    // ---------------------------------------------------------------------------------
    // telephony.registry: ITelephonyRegistry (subscription and phone state listeners).
    // ---------------------------------------------------------------------------------

    static final class Registry extends ITelephonyRegistry.Default {
        private static final Set<Integer> PRECISE = Set.of(
                TelephonyCallback.EVENT_PRECISE_DATA_CONNECTION_STATE_CHANGED,
                TelephonyCallback.EVENT_DATA_CONNECTION_REAL_TIME_INFO_CHANGED,
                TelephonyCallback.EVENT_PRECISE_CALL_STATE_CHANGED,
                TelephonyCallback.EVENT_CALL_DISCONNECT_CAUSE_CHANGED,
                TelephonyCallback.EVENT_CALL_ATTRIBUTES_CHANGED,
                TelephonyCallback.EVENT_IMS_CALL_DISCONNECT_CAUSE_CHANGED,
                TelephonyCallback.EVENT_REGISTRATION_FAILURE,
                TelephonyCallback.EVENT_BARRING_INFO_CHANGED,
                TelephonyCallback.EVENT_PHYSICAL_CHANNEL_CONFIG_CHANGED,
                TelephonyCallback.EVENT_DATA_ENABLED_CHANGED,
                TelephonyCallback.EVENT_LINK_CAPACITY_ESTIMATE_CHANGED,
                TelephonyCallback.EVENT_MEDIA_QUALITY_STATUS_CHANGED);
        private static final Set<Integer> PRIVILEGED = Set.of(
                TelephonyCallback.EVENT_SRVCC_STATE_CHANGED,
                TelephonyCallback.EVENT_VOICE_ACTIVATION_STATE_CHANGED,
                TelephonyCallback.EVENT_RADIO_POWER_STATE_CHANGED,
                TelephonyCallback.EVENT_ALLOWED_NETWORK_TYPE_LIST_CHANGED,
                TelephonyCallback.EVENT_EMERGENCY_CALLBACK_MODE_CHANGED,
                TelephonyCallback.EVENT_SIMULTANEOUS_CELLULAR_CALLING_SUBSCRIPTIONS_CHANGED,
                TelephonyCallback.EVENT_CELLULAR_IDENTIFIER_DISCLOSED_CHANGED,
                TelephonyCallback.EVENT_SECURITY_ALGORITHMS_CHANGED);

        private final Cellular mCellular;
        private final Rules mRules;
        private final Object mLock = new Object();
        private final Map<IBinder, Record> mRecords = new HashMap<>();
        private final List<IOnSubscriptionsChangedListener> mSubscriptionListeners = new ArrayList<>();
        // The values last delivered, so listeners only hear real changes.
        private ServiceState mServiceState;
        private WindowsCellular.Modem mModem;
        private SignalStrength mSignal;
        private int mDataState = -1, mDataType = -1, mSubId;

        private static final class Record {
            IPhoneStateListener callback;
            Caller caller;
            Set<Integer> events;
            int subId;
            boolean renounceFine, renounceCoarse;
        }

        Registry(Cellular cellular, Rules rules) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
            mSubId = cellular.subId();
            mModem = cellular.modem();
            mServiceState = cellular.present() ? WindowsCellular.serviceState(mModem) : null;
            mSignal = cellular.present() ? signal() : null;
            mDataState = WindowsCellular.dataState(cellular.modem());
            mDataType = WindowsCellular.dataNetworkType(cellular.modem());
            cellular.addListener(this::onModemChanged);
        }

        private SignalStrength signal() {
            WindowsCellular.Modem modem = mCellular.modem();
            return modem == null ? new SignalStrength() : WindowsCellular.signalStrength(modem);
        }

        /**
         * TelephonyRegistry tells a new listener the current subscriptions at once (they
         * were published at boot); SubscriptionManager users wait for that first call.
         */
        @Override
        public void addOnSubscriptionsChangedListener(String callingPackage, String callingFeatureId,
                IOnSubscriptionsChangedListener callback) {
            if (callback == null) return;
            synchronized (mLock) {
                mSubscriptionListeners.add(callback);
            }
            try {
                callback.onSubscriptionsChanged();
            } catch (RemoteException | RuntimeException e) {
                removeSubscriptionListener(callback);
            }
        }

        @Override
        public void removeOnSubscriptionsChangedListener(String callingPackage,
                IOnSubscriptionsChangedListener callback) {
            removeSubscriptionListener(callback);
        }

        private void removeSubscriptionListener(IOnSubscriptionsChangedListener callback) {
            if (callback == null) return;
            synchronized (mLock) {
                mSubscriptionListeners.removeIf(l -> l.asBinder() == callback.asBinder());
            }
        }

        /** checkListenerPermission: SecurityException, false (app op revoked) or true. */
        private boolean permitted(Set<Integer> events, Caller c) {
            boolean ok = true;
            if ((events.contains(TelephonyCallback.EVENT_CELL_LOCATION_CHANGED)
                    || events.contains(TelephonyCallback.EVENT_CELL_INFO_CHANGED))
                    && !mRules.anyLocationPermission(c)) {
                throw new SecurityException("Unable to listen for events " + events + " due to "
                        + "insufficient location permissions.");
            }
            boolean s = c.targetSdk() >= Build.VERSION_CODES.S;
            boolean phoneState = events.contains(TelephonyCallback.EVENT_CALL_FORWARDING_INDICATOR_CHANGED)
                    || events.contains(TelephonyCallback.EVENT_MESSAGE_WAITING_INDICATOR_CHANGED)
                    || events.contains(TelephonyCallback.EVENT_EMERGENCY_NUMBER_LIST_CHANGED)
                    || s && (events.contains(TelephonyCallback.EVENT_LEGACY_CALL_STATE_CHANGED)
                            || events.contains(TelephonyCallback.EVENT_CALL_STATE_CHANGED)
                            || events.contains(TelephonyCallback.EVENT_ACTIVE_DATA_SUBSCRIPTION_ID_CHANGED)
                            || events.contains(TelephonyCallback.EVENT_CELL_INFO_CHANGED))
                    || !s && events.contains(TelephonyCallback.EVENT_DISPLAY_INFO_CHANGED);
            if (phoneState && !mRules.readPhoneState(c, "listen")) ok = false;
            for (int event : events) {
                if (PRECISE.contains(event) && !mRules.granted(c, READ_PRECISE_PHONE_STATE)) {
                    throw new SecurityException("listen: uid " + c.uid() + " requires "
                            + READ_PRECISE_PHONE_STATE + " or carrier privileges");
                }
                if (PRIVILEGED.contains(event) && !mRules.granted(c, READ_PRIVILEGED_PHONE_STATE)) {
                    throw new SecurityException("listen: uid " + c.uid() + " requires "
                            + READ_PRIVILEGED_PHONE_STATE);
                }
            }
            if ((events.contains(TelephonyCallback.EVENT_OUTGOING_EMERGENCY_CALL)
                    || events.contains(TelephonyCallback.EVENT_OUTGOING_EMERGENCY_SMS))
                    && !mRules.granted(c, READ_ACTIVE_EMERGENCY_SESSION)) {
                throw new SecurityException("listen: uid " + c.uid() + " requires "
                        + READ_ACTIVE_EMERGENCY_SESSION);
            }
            return ok;
        }

        /**
         * Phone and data events from the modem. Only events Gesher can report are ever
         * delivered: service state, signal strength(s), data connection state and the
         * (always idle) call state; the rest are accepted, permission-checked and silent.
         */
        @Override
        public void listenWithEventList(boolean renounceFine, boolean renounceCoarse, int subId,
                String callingPackage, String callingFeatureId, IPhoneStateListener callback,
                int[] events, boolean notifyNow) {
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (callback == null) return;
            Set<Integer> eventSet = new HashSet<>();
            if (events != null) for (int e : events) eventSet.add(e);
            IBinder binder = callback.asBinder();
            if (eventSet.isEmpty()) {
                synchronized (mLock) { mRecords.remove(binder); }
                return;
            }
            if (!permitted(eventSet, c)) return;
            Record r = new Record();
            r.callback = callback;
            r.caller = c;
            r.events = eventSet;
            r.subId = SubscriptionManager.isValidSubscriptionId(subId) ? subId
                    : SubscriptionManager.DEFAULT_SUBSCRIPTION_ID;
            r.renounceFine = renounceFine;
            r.renounceCoarse = renounceCoarse;
            synchronized (mLock) {
                mRecords.put(binder, r);
                if (notifyNow && mCellular.phoneForSub(r.subId) == 0) {
                    initial(r);
                }
            }
        }

        // Called with mLock.
        private void initial(Record r) {
            try {
                if (r.events.contains(TelephonyCallback.EVENT_SERVICE_STATE_CHANGED)
                        && mServiceState != null) {
                    r.callback.onServiceStateChanged(serviceStateFor(r));
                }
                if (r.events.contains(TelephonyCallback.EVENT_SIGNAL_STRENGTH_CHANGED) && mSignal != null) {
                    int gsm = mSignal.getGsmSignalStrength();
                    r.callback.onSignalStrengthChanged(gsm == 99 ? -1 : gsm);
                }
                if (r.events.contains(TelephonyCallback.EVENT_LEGACY_CALL_STATE_CHANGED)) {
                    r.callback.onLegacyCallStateChanged(TelephonyManager.CALL_STATE_IDLE, "");
                }
                if (r.events.contains(TelephonyCallback.EVENT_CALL_STATE_CHANGED)) {
                    r.callback.onCallStateChanged(TelephonyManager.CALL_STATE_IDLE);
                }
                if (r.events.contains(TelephonyCallback.EVENT_DATA_CONNECTION_STATE_CHANGED)) {
                    r.callback.onDataConnectionStateChanged(mDataState, mDataType);
                }
                if (r.events.contains(TelephonyCallback.EVENT_SIGNAL_STRENGTHS_CHANGED) && mSignal != null) {
                    r.callback.onSignalStrengthsChanged(new SignalStrength(mSignal));
                }
            } catch (RemoteException | RuntimeException e) {
                mRecords.remove(r.callback.asBinder());
            }
        }

        private ServiceState serviceStateFor(Record r) {
            boolean fine = !r.renounceFine && mRules.fineLocation(r.caller);
            boolean coarse = !r.renounceCoarse && mRules.coarseLocation(r.caller);
            return forCaller(mModem, fine, coarse);
        }

        private void onModemChanged() {
            List<IOnSubscriptionsChangedListener> subscriptionListeners;
            synchronized (mLock) {
                WindowsCellular.Modem modem = mCellular.modem();
                ServiceState state = WindowsCellular.serviceState(modem);
                SignalStrength signal = modem == null ? new SignalStrength()
                        : WindowsCellular.signalStrength(modem);
                int dataState = WindowsCellular.dataState(modem);
                int dataType = WindowsCellular.dataNetworkType(modem);
                boolean stateChanged = !state.equals(mServiceState);
                boolean signalChanged = !signal.equals(mSignal);
                boolean dataChanged = dataState != mDataState || dataType != mDataType;
                int subId = mCellular.subId();
                boolean subscriptionsChanged = subId != mSubId;
                mServiceState = state;
                mModem = modem;
                mSignal = signal;
                mDataState = dataState;
                mDataType = dataType;
                mSubId = subId;
                for (Record r : new ArrayList<>(mRecords.values())) {
                    // A default-subscription listener follows phone 0, the only phone.
                    if (r.subId != SubscriptionManager.DEFAULT_SUBSCRIPTION_ID && r.subId != subId) {
                        continue;
                    }
                    try {
                        if (stateChanged && r.events.contains(TelephonyCallback.EVENT_SERVICE_STATE_CHANGED)) {
                            r.callback.onServiceStateChanged(serviceStateFor(r));
                        }
                        if (signalChanged && r.events.contains(TelephonyCallback.EVENT_SIGNAL_STRENGTH_CHANGED)) {
                            int gsm = signal.getGsmSignalStrength();
                            r.callback.onSignalStrengthChanged(gsm == 99 ? -1 : gsm);
                        }
                        if (signalChanged && r.events.contains(TelephonyCallback.EVENT_SIGNAL_STRENGTHS_CHANGED)) {
                            r.callback.onSignalStrengthsChanged(new SignalStrength(signal));
                        }
                        if (dataChanged && r.events.contains(TelephonyCallback.EVENT_DATA_CONNECTION_STATE_CHANGED)) {
                            r.callback.onDataConnectionStateChanged(dataState, dataType);
                        }
                    } catch (RemoteException | RuntimeException e) {
                        mRecords.remove(r.callback.asBinder());
                    }
                }
                subscriptionListeners = subscriptionsChanged
                        ? new ArrayList<>(mSubscriptionListeners) : Collections.emptyList();
            }
            for (IOnSubscriptionsChangedListener listener : subscriptionListeners) {
                try {
                    listener.onSubscriptionsChanged();
                } catch (RemoteException | RuntimeException e) {
                    removeSubscriptionListener(listener);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------
    // Registration and system features.
    // ---------------------------------------------------------------------------------

    /** The four services over one modem model. */
    static final class Endpoints {
        final Cellular cellular;
        final Phone phone;
        final PhoneSubInfo phoneSubInfo;
        final Subscriptions subscriptions;
        final Registry registry;

        Endpoints(Cellular cellular, Callers callers) {
            Rules rules = new Rules(callers);
            this.cellular = cellular;
            phone = new Phone(cellular, rules);
            phoneSubInfo = new PhoneSubInfo(cellular, rules);
            subscriptions = new Subscriptions(cellular, rules);
            registry = new Registry(cellular, rules);
        }
    }

    /** Production services, over the boot snapshot of Windows' Mobile Broadband service. */
    static Endpoints create(PackageManagerService pm, ActivityManagerService am,
            BooleanSupplier locationEnabled) {
        Cellular cellular = new Cellular(WindowsCellular.WINDOWS, bootSnapshot(), SYSTEM_PROPERTIES);
        Endpoints bundle = new Endpoints(cellular, callers(pm, am, locationEnabled));
        cellular.start();
        return bundle;
    }

    /**
     * Telephony features for a modem present at boot: telephony, subscription, radio
     * access and data, plus GSM for a 3GPP (GSM-class) modem. Never calling, messaging,
     * IMS, eUICC or CDMA (Android 16 no longer supports legacy CDMA). No modem, none.
     */
    static void addTelephonyFeatures(android.util.ArrayMap<String, android.content.pm.FeatureInfo> features,
            WindowsCellular.Snapshot boot) {
        WindowsCellular.Modem modem = boot == null ? null : boot.primary();
        if (modem == null) return;
        List<String> names = new ArrayList<>(List.of(PackageManager.FEATURE_TELEPHONY,
                PackageManager.FEATURE_TELEPHONY_SUBSCRIPTION,
                PackageManager.FEATURE_TELEPHONY_RADIO_ACCESS,
                PackageManager.FEATURE_TELEPHONY_DATA));
        if (modem.cellularClass == WindowsCellular.CLASS_GSM) names.add(PackageManager.FEATURE_TELEPHONY_GSM);
        for (String name : names) {
            android.content.pm.FeatureInfo f = new android.content.pm.FeatureInfo();
            f.name = name;
            features.put(name, f);
        }
    }
}
