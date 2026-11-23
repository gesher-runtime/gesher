/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Messenger;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.os.WorkSource;
import android.sysprop.TelephonyProperties;
import android.net.Uri;
import android.telephony.CellIdentity;
import android.telephony.CellIdentityGsm;
import android.telephony.CellInfo;
import android.telephony.IBootstrapAuthenticationCallback;
import android.telephony.ICellInfoCallback;
import android.telephony.NetworkScanRequest;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyDisplayInfo;
import android.telephony.TelephonyManager;
import android.telephony.TelephonyScanManager;
import android.telephony.UiccCardInfo;
import android.telephony.UiccPortInfo;
import android.telephony.UssdResponse;
import android.telephony.gba.UaSecurityProtocolIdentifier;
import com.android.internal.telephony.IIntegerConsumer;
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
    static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";
    static final String MODIFY_PHONE_STATE = "android.permission.MODIFY_PHONE_STATE";
    static final String CALL_PHONE = "android.permission.CALL_PHONE";
    static final String INTERNET = "android.permission.INTERNET";
    static final String PERFORM_IMS_SINGLE_REGISTRATION =
            "android.permission.PERFORM_IMS_SINGLE_REGISTRATION";
    /** The null-service message of most TelephonyManager methods that throw. */
    static final String TELEPHONY_SERVICE_NULL = "telephony service is null.";

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

    /**
     * The modem behind Android phone 0, if one was present at boot.
     *
     * <p>Readings are demand-driven. Each read of Windows' Mobile Broadband service creates
     * an out-of-process WwanSvc object and makes about a dozen calls to it, so the modem is
     * polled every {@link #POLL_MILLIS} only while telephony is in use: while
     * telephony.registry holds a listener, or for {@link #IDLE_MILLIS} after the last query.
     * A query finding the reading older than {@link #FRESH_MILLIS} (polling had stopped)
     * reads the modem before answering, so an answer never presents an old reading as
     * current. Every read is bounded by {@link #READ_TIMEOUT_MILLIS}; a read that fails or
     * times out keeps the last good reading only for the next two reads and only while that
     * reading is under {@link #KEEP_MILLIS} old, after which phone 0 has no reading
     * (radio-off service state, SIM not ready) until a read succeeds.
     */
    static final class Cellular {
        static final long POLL_MILLIS = 5000;
        static final long IDLE_MILLIS = 30000;
        static final long READ_TIMEOUT_MILLIS = 2000;
        static final long FRESH_MILLIS = POLL_MILLIS + READ_TIMEOUT_MILLIS;
        static final int MAX_FAILED_READS = 3;
        static final long KEEP_MILLIS = MAX_FAILED_READS * POLL_MILLIS;

        /** A monotonic clock in milliseconds. */
        interface Clock {
            long millis();
        }

        /** Runs the polling loop; production uses a daemon thread. */
        interface Workers {
            void start(Runnable loop);
        }

        static final Clock MONOTONIC = () -> System.nanoTime() / 1_000_000L;
        static final Workers DAEMON = loop -> {
            Thread worker = new Thread(loop, "gesher-cellular");
            worker.setDaemon(true);
            worker.start();
        };

        private final WindowsCellular.Source mSource;
        private final PropertySink mProperties;
        private final Clock mClock;
        private final Workers mWorkers;
        private final long mReadTimeoutMillis;
        private final boolean mPresent;
        private final String mInterfaceId;
        private final Object mLock = new Object();
        // Serializes reads, so concurrent queries share one read instead of starting more.
        private final Object mReadLock = new Object();
        private final List<Runnable> mListeners = new CopyOnWriteArrayList<>();
        // Subscription IDs are assigned per ICCID for this boot, from 1 as Android's
        // subscription database assigns them; they are not persisted across restarts.
        private final Map<String, Integer> mSubIds = new HashMap<>();
        private int mNextSubId = 1;
        private WindowsCellular.Modem mModem;
        private int mSubId = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
        private int mFailures;
        private long mReadAt, mGoodAt, mDemandAt;
        private boolean mWatched, mPolling;
        // A read still blocked in WwanSvc after its timeout (with mReadLock); no second
        // read is started while it lasts.
        private Thread mStuckRead;

        Cellular(WindowsCellular.Source source, WindowsCellular.Snapshot boot, PropertySink properties) {
            this(source, boot, properties, MONOTONIC, DAEMON, READ_TIMEOUT_MILLIS);
        }

        Cellular(WindowsCellular.Source source, WindowsCellular.Snapshot boot, PropertySink properties,
                Clock clock, Workers workers, long readTimeoutMillis) {
            mSource = Objects.requireNonNull(source);
            mProperties = Objects.requireNonNull(properties);
            mClock = Objects.requireNonNull(clock);
            mWorkers = Objects.requireNonNull(workers);
            mReadTimeoutMillis = readTimeoutMillis;
            WindowsCellular.Modem modem = boot == null ? null : boot.primary();
            mPresent = modem != null;
            mInterfaceId = mPresent ? modem.interfaceId : null;
            long now = clock.millis();
            synchronized (mLock) {
                mReadAt = mGoodAt = now;
                mDemandAt = now - IDLE_MILLIS;
                if (mPresent) {
                    mModem = modem;
                    mSubId = subscriptionFor(modem);
                }
            }
            if (mPresent) publish(modem);
        }

        /** Whether phone 0 exists (fixed at boot). */
        boolean present() { return mPresent; }

        /**
         * The current reading of the boot modem, for a query: null if it has gone or cannot
         * be read. Reads the modem first when the last reading is no longer current.
         */
        WindowsCellular.Modem modem() {
            query();
            return reading();
        }

        /** The active subscription's ID, for a query; INVALID_SUBSCRIPTION_ID without a ready SIM. */
        int subId() {
            query();
            return readingSubId();
        }

        /** The last reading, without counting as a query (change listeners use it). */
        WindowsCellular.Modem reading() {
            synchronized (mLock) { return mModem; }
        }

        int readingSubId() {
            synchronized (mLock) { return mSubId; }
        }

        int simState() {
            if (!mPresent) return TelephonyManager.SIM_STATE_UNKNOWN;
            WindowsCellular.Modem modem = modem();
            return modem == null ? TelephonyManager.SIM_STATE_NOT_READY : WindowsCellular.simState(modem);
        }

        void addListener(Runnable listener) { mListeners.add(listener); }

        /** Whether telephony.registry has listeners; polling continues while it has. */
        void setWatched(boolean watched) {
            if (!mPresent) return;
            synchronized (mLock) {
                if (mWatched == watched) return;
                mWatched = watched;
            }
            if (watched) ensurePolling();
        }

        /** Whether the polling worker is scheduled (for tests). */
        boolean polling() {
            synchronized (mLock) { return mPolling; }
        }

        /** A query: keeps polling alive and makes sure the reading it answers from is current. */
        private void query() {
            if (!mPresent) return;
            synchronized (mLock) {
                mDemandAt = mClock.millis();
            }
            ensurePolling();
            if (read(FRESH_MILLIS)) notifyListeners();
        }

        private void ensurePolling() {
            synchronized (mLock) {
                if (mPolling) return;
                mPolling = true;
            }
            try {
                mWorkers.start(this::pollLoop);
            } catch (RuntimeException | Error e) {
                synchronized (mLock) { mPolling = false; }
                throw e;
            }
        }

        private void pollLoop() {
            while (true) {
                try {
                    Thread.sleep(POLL_MILLIS);
                } catch (InterruptedException e) {
                    synchronized (mLock) { mPolling = false; }
                    return;
                }
                if (!pollOnce()) return;
            }
        }

        /**
         * One scheduled poll. Returns false, and polling stops, when no listener is
         * registered and nothing has queried telephony for IDLE_MILLIS.
         */
        boolean pollOnce() {
            synchronized (mLock) {
                if (!mWatched && mClock.millis() - mDemandAt >= IDLE_MILLIS) {
                    mPolling = false;
                    return false;
                }
            }
            refresh();
            return true;
        }

        /** One bounded read; listeners hear about changes. Returns whether anything changed. */
        boolean refresh() {
            boolean changed = read(-1);
            if (changed) notifyListeners();
            return changed;
        }

        /**
         * Reads the modem unless (minAge >= 0) the reading is younger than minAge, which
         * another caller may have just refreshed. Returns whether the model changed.
         */
        private boolean read(long minAge) {
            if (!mPresent) return false;
            synchronized (mReadLock) {
                if (minAge >= 0) {
                    synchronized (mLock) {
                        if (mClock.millis() - mReadAt < minAge) return false;
                    }
                }
                WindowsCellular.Snapshot snapshot = WindowsCellular.parse(fetch());
                WindowsCellular.Modem next = null;
                boolean readable = snapshot != null && !"error".equals(snapshot.status);
                if (readable) {
                    for (WindowsCellular.Modem modem : snapshot.modems) {
                        if (modem.interfaceId.equals(mInterfaceId)) next = modem;
                    }
                }
                synchronized (mLock) {
                    long now = mClock.millis();
                    mReadAt = now;
                    if (!readable) {
                        // A transient failure keeps a recent reading for two more reads; an
                        // old one, or a third failure, is not presented as current.
                        if (++mFailures < MAX_FAILED_READS && now - mGoodAt < KEEP_MILLIS) return false;
                    } else {
                        mFailures = 0;
                        mGoodAt = now;
                    }
                    if (Objects.equals(mModem == null ? null : mModem.source,
                            next == null ? null : next.source)) {
                        return false;
                    }
                    mModem = next;
                    mSubId = subscriptionFor(next);
                }
                publish(next);
                return true;
            }
        }

        /** One snapshot, or null when it fails or exceeds the timeout. Called with mReadLock. */
        private String fetch() {
            if (mStuckRead != null) {
                if (mStuckRead.isAlive()) return null;
                mStuckRead = null;
            }
            AtomicReference<String> raw = new AtomicReference<>();
            Thread reader = new Thread(() -> raw.set(mSource.snapshot()), "gesher-cellular-read");
            reader.setDaemon(true);
            reader.start();
            try {
                reader.join(mReadTimeoutMillis);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (reader.isAlive()) {
                // Its late result is discarded: it would no longer be current.
                mStuckRead = reader;
                return null;
            }
            return raw.get();
        }

        private void notifyListeners() {
            for (Runnable listener : mListeners) {
                try {
                    listener.run();
                } catch (RuntimeException e) {
                    // One observer's failure does not stop others.
                }
            }
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

        /**
         * SubscriptionManagerService.getPhoneId: the default subscription's phone for
         * DEFAULT_SUBSCRIPTION_ID, INVALID_PHONE_INDEX for an invalid ID, the slot of the
         * active subscription, and DEFAULT_PHONE_INDEX for a valid ID without a slot.
         */
        int phoneId(int subId) {
            int active = subId();
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = active;
            if (!SubscriptionManager.isValidSubscriptionId(subId)) return SubscriptionManager.INVALID_PHONE_INDEX;
            return subId == active ? 0 : SubscriptionManager.DEFAULT_PHONE_INDEX;
        }

        /**
         * PhoneFactory.getPhone(index) != null: phone 0 itself or DEFAULT_PHONE_INDEX (the
         * default phone), while a modem was present at boot.
         */
        boolean phone(int phoneId) {
            return mPresent && (phoneId == 0 || phoneId == SubscriptionManager.DEFAULT_PHONE_INDEX);
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
            // checkCallingOrSelfUseIccAuthWithDeviceIdentifier: MODE_FOREGROUND allows too.
            if (icc == AppOpsManager.MODE_ALLOWED || icc == AppOpsManager.MODE_FOREGROUND
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

        boolean anyGranted(Caller c, String... permissions) {
            for (String permission : permissions) {
                if (granted(c, permission)) return true;
            }
            return false;
        }

        /**
         * LocationAccessPolicy.checkLocationPermission (android-16.0.0_r4): the system is
         * allowed; the location switch off is a soft denial; then fine (if minFine is below
         * MAX_VALUE) and coarse are checked in that order, each answering ALLOWED for the
         * permission with its app op allowed, HARD for MODE_ERRORED, SOFT for another mode,
         * HARD for no permission at a target at or above its minimum, and no answer below it.
         */
        Location location(Caller c, int minFine, int minCoarse) {
            if (c.system()) return Location.ALLOWED;
            if (!mCallers.locationEnabled()) return Location.DENIED_SOFT;
            if (minFine < Integer.MAX_VALUE) {
                Location fine = location(c, FINE, AppOpsManager.OPSTR_FINE_LOCATION, minFine);
                if (fine != null) return fine;
            }
            if (minCoarse < Integer.MAX_VALUE) {
                Location coarse = location(c, COARSE, AppOpsManager.OPSTR_COARSE_LOCATION, minCoarse);
                if (coarse != null) return coarse;
            }
            return Location.ALLOWED;
        }

        private Location location(Caller c, String permission, String op, int minSdk) {
            if (granted(c, permission)) {
                int mode = mCallers.noteOp(c, op);
                if (mode == AppOpsManager.MODE_ALLOWED) return Location.ALLOWED;
                return mode == AppOpsManager.MODE_ERRORED ? Location.DENIED_HARD : Location.DENIED_SOFT;
            }
            if (minSdk > Build.VERSION_CODES.CUR_DEVELOPMENT || c.targetSdk() < minSdk) return null;
            return Location.DENIED_HARD;
        }
    }

    /** LocationAccessPolicy.LocationPermissionResult. */
    enum Location { ALLOWED, DENIED_SOFT, DENIED_HARD }

    /**
     * ServiceState as a caller may see it: the registered operator is location data
     * (AOSP sanitizes it without coarse location). Fine location adds nothing here,
     * since no cell identity is reported. Both answers come from LocationAccessPolicy with
     * minimum SDK Q (PhoneInterfaceManager.getServiceStateForSlot and TelephonyRegistry
     * alike), so an app targeting below Q keeps the operator without a location permission.
     */
    static ServiceState forCaller(WindowsCellular.Modem modem, boolean fine, boolean coarse) {
        return WindowsCellular.serviceState(modem, fine || coarse);
    }

    // ---------------------------------------------------------------------------------
    // Android's mobile data switches (DataSettingsManager), over Settings.Global.
    // ---------------------------------------------------------------------------------

    /** Settings.Global as the phone process reads and writes it. */
    interface GlobalSettings {
        /** The value, or null when the setting is absent. */
        String get(String name);
        void put(String name, String value);
    }

    /** The system server's settings provider: durable rows, and observers hear changes. */
    static final GlobalSettings SETTINGS_PROVIDER = new GlobalSettings() {
        @Override
        public String get(String name) {
            return SettingsProvider.globalString(name);
        }

        @Override
        public void put(String name, String value) {
            if (!SettingsProvider.putGlobal(name, value)) {
                throw new IllegalStateException("Settings provider unavailable");
            }
        }
    };

    /**
     * A store of its own, for endpoints created without one (host scenarios), holding the
     * rows SettingsProvider starts a device with for these switches.
     */
    static GlobalSettings memorySettings() {
        Map<String, String> values = new java.util.concurrent.ConcurrentHashMap<>();
        values.put(android.provider.Settings.Global.DEVICE_PROVISIONED, "1");
        values.put(android.provider.Settings.Global.MOBILE_DATA, "1");
        values.put(android.provider.Settings.Global.DATA_ROAMING, "0");
        return new GlobalSettings() {
            @Override public String get(String name) { return values.get(name); }
            @Override public void put(String name, String value) { values.put(name, value); }
        };
    }

    /**
     * Phone 0's data switches, as AOSP's DataSettingsManager keeps them (android-16.0.0_r4).
     * The user switch and data roaming are Settings.Global.MOBILE_DATA and DATA_ROAMING,
     * unsuffixed as GlobalSettingsHelper names them on a single-SIM device; they are read
     * on every query (a value written to Settings.Global directly is the answer too) and
     * persist as every Gesher setting does. Their defaults are AOSP's: mobile data on
     * (ro.com.android.mobiledata unset), roaming off (ro.com.android.dataroaming unset, no
     * carrier configuration). Before the device is provisioned,
     * DEVICE_PROVISIONING_MOBILE_DATA_ENABLED decides. The policy, carrier and thermal
     * switches are kept in memory from true, as DataSettingsManager keeps them.
     *
     * <p>These are Android's switches. Windows decides whether its own cellular connection
     * is up and carries traffic (Gesher never connects or disconnects it), so turning them
     * off does not stop Android traffic over the modem; getDataState and ConnectivityManager
     * keep reporting the connection Windows has (docs/telephony.md).
     */
    static final class DataSettings {
        /** Hears a change of the user or overall switch, with the reason of the change. */
        interface Listener {
            void onChanged(int reason);
        }

        private final GlobalSettings mSettings;
        private final Object mLock = new Object();
        // DataSettingsManager.mDataEnabledSettings: policy, carrier and thermal.
        private final Map<Integer, Boolean> mReasons = new HashMap<>();
        private final List<Listener> mListeners = new CopyOnWriteArrayList<>();

        DataSettings(GlobalSettings settings) {
            mSettings = Objects.requireNonNull(settings);
            mReasons.put(TelephonyManager.DATA_ENABLED_REASON_POLICY, true);
            mReasons.put(TelephonyManager.DATA_ENABLED_REASON_CARRIER, true);
            mReasons.put(TelephonyManager.DATA_ENABLED_REASON_THERMAL, true);
        }

        void addListener(Listener listener) { mListeners.add(listener); }

        /** Settings.Global.getInt(resolver, name, default): an unparsable value is the default. */
        private int getInt(String name, int defaultValue) {
            String value = mSettings.get(name);
            if (value == null) return defaultValue;
            try {
                return Integer.parseInt(value);
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        /** GlobalSettingsHelper.setInt: writes, and answers true, unless the value is already set. */
        private boolean setInt(String name, int value) {
            String current = mSettings.get(name);
            try {
                if (current != null && Integer.parseInt(current) == value) return false;
            } catch (NumberFormatException e) {
                // Settings.Global.getInt throws SettingNotFoundException: written anew.
            }
            mSettings.put(name, Integer.toString(value));
            return true;
        }

        private boolean provisioned() {
            return getInt(android.provider.Settings.Global.DEVICE_PROVISIONED, 0) != 0;
        }

        /** isProvisioningDataEnabled. */
        private boolean provisioningDataEnabled() {
            boolean property = "true".equalsIgnoreCase(
                    android.os.SystemProperties.get("ro.com.android.prov_mobiledata", "false"));
            return getInt(android.provider.Settings.Global.DEVICE_PROVISIONING_MOBILE_DATA_ENABLED,
                    property ? 1 : 0) != 0;
        }

        /** isUserDataEnabled: Settings.Global.MOBILE_DATA, default ro.com.android.mobiledata (on). */
        boolean userEnabled() {
            if (!provisioned()) return provisioningDataEnabled();
            boolean defaultValue = TelephonyProperties.mobile_data().orElse(true);
            return getInt(android.provider.Settings.Global.MOBILE_DATA, defaultValue ? 1 : 0) == 1;
        }

        /**
         * isDataEnabledForReason: the user switch, or the policy, carrier or thermal one.
         * Another reason has no entry, and DataSettingsManager's unboxing of the missing entry
         * throws NullPointerException, which Binder carries to the caller.
         */
        boolean enabledForReason(int reason) {
            if (reason == TelephonyManager.DATA_ENABLED_REASON_USER) return userEnabled();
            Boolean enabled;
            synchronized (mLock) {
                enabled = mReasons.get(reason);
            }
            if (enabled == null) throw new NullPointerException("No data enabled reason " + reason);
            return enabled;
        }

        /** isDataEnabled(TYPE_ALL): every switch on (no mobile data policy overrides exist). */
        boolean enabled() {
            if (!provisioned()) return provisioningDataEnabled();
            synchronized (mLock) {
                if (!mReasons.get(TelephonyManager.DATA_ENABLED_REASON_POLICY)
                        || !mReasons.get(TelephonyManager.DATA_ENABLED_REASON_CARRIER)
                        || !mReasons.get(TelephonyManager.DATA_ENABLED_REASON_THERMAL)) {
                    return false;
                }
            }
            return userEnabled();
        }

        /** isDataRoamingEnabled: Settings.Global.DATA_ROAMING, default ro.com.android.dataroaming (off). */
        boolean roamingEnabled() {
            boolean defaultValue = "true".equalsIgnoreCase(
                    android.os.SystemProperties.get("ro.com.android.dataroaming", "false"));
            return getInt(android.provider.Settings.Global.DATA_ROAMING, defaultValue ? 1 : 0) == 1;
        }

        /**
         * DataSettingsManager.setDataEnabled for the user, policy or thermal reason (the
         * carrier reason is a carrier action, see Phone.setDataEnabledForReason); another
         * reason is ignored, as it is logged and ignored there. Listeners hear a change.
         */
        void setEnabled(int reason, boolean enabled) {
            boolean changed;
            synchronized (mLock) {
                if (reason == TelephonyManager.DATA_ENABLED_REASON_USER) {
                    changed = setInt(android.provider.Settings.Global.MOBILE_DATA, enabled ? 1 : 0);
                } else if (reason == TelephonyManager.DATA_ENABLED_REASON_POLICY
                        || reason == TelephonyManager.DATA_ENABLED_REASON_THERMAL) {
                    changed = mReasons.put(reason, enabled) != enabled;
                } else {
                    return;
                }
            }
            if (!changed) return;
            for (Listener listener : mListeners) {
                try {
                    listener.onChanged(reason);
                } catch (RuntimeException e) {
                    // One observer's failure does not stop others.
                }
            }
        }

        /** setDataRoamingEnabledInternal: Settings.Global.DATA_ROAMING (no registry event exists). */
        void setRoamingEnabled(boolean enabled) {
            synchronized (mLock) {
                setInt(android.provider.Settings.Global.DATA_ROAMING, enabled ? 1 : 0);
            }
        }
    }

    // ---------------------------------------------------------------------------------
    // phone: ITelephony.
    // ---------------------------------------------------------------------------------

    static final class Phone extends ITelephony.Default {
        private final Cellular mCellular;
        private final Rules mRules;
        private final DataSettings mData;

        Phone(Cellular cellular, Rules rules, DataSettings data) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
            mData = Objects.requireNonNull(data);
        }

        private WindowsCellular.Modem modemForSub(int subId) {
            return mCellular.phoneForSub(subId) == 0 ? mCellular.modem() : null;
        }

        /**
         * The SIM state in a slot: the modem's, UNKNOWN for a slot that is not phone 0.
         * Without a modem, UNKNOWN, what AOSP's TelephonyManager.getSimStateForSlotIndex
         * answers on a device without telephony, where ITelephony is null (ADR 0094,
         * superseding ADR 0087's ABSENT). TelephonyManager.getSimState() asks for
         * DEFAULT_SIM_SLOT_INDEX when there is no default subscription, so this answer is
         * also getSimState()'s (docs/telephony.md).
         */
        @Override
        public int getSimStateForSlotIndex(int slotIndex) {
            if (slotIndex < 0 || !mCellular.present()) return TelephonyManager.SIM_STATE_UNKNOWN;
            return slot(slotIndex) ? mCellular.simState() : TelephonyManager.SIM_STATE_UNKNOWN;
        }

        /**
         * Whether a slot index names phone 0: 0 itself, or DEFAULT_SIM_SLOT_INDEX, which is
         * PhoneFactory.getPhone's DEFAULT_PHONE_INDEX (the default phone).
         */
        private boolean slot(int slotIndex) {
            return mCellular.phone(slotIndex);
        }

        /**
         * UiccProfile.hasIccCard: a card is in the slot whatever its lock or error state.
         * Without a phone, false (TelephonyManager's null-service answer too).
         */
        @Override
        public boolean hasIccCardUsingSlotIndex(int slotIndex) {
            return slot(slotIndex) && WindowsCellular.cardPresent(mCellular.modem());
        }

        @Override
        public boolean hasIccCard() {
            return hasIccCardUsingSlotIndex(0);
        }

        /** The registered network's country; "" without a phone or registration. */
        @Override
        public String getNetworkCountryIsoForPhone(int phoneId) {
            if (phoneId == SubscriptionManager.INVALID_PHONE_INDEX) {
                phoneId = SubscriptionManager.DEFAULT_PHONE_INDEX;
            }
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
            boolean fine = !renounceFine && mRules.location(c, Build.VERSION_CODES.Q,
                    Build.VERSION_CODES.Q) == Location.ALLOWED;
            boolean coarse = !renounceCoarse && mRules.location(c, Integer.MAX_VALUE,
                    Build.VERSION_CODES.Q) == Location.ALLOWED;
            return forCaller(mCellular.modem(), fine, coarse);
        }

        @Override
        public boolean isRadioOn(String callingPackage) {
            return isRadioOnWithFeature(callingPackage, null);
        }

        @Override
        public boolean isRadioOnForSubscriber(int subId, String callingPackage) {
            return isRadioOnForSubscriberWithFeature(subId, callingPackage, null);
        }

        /**
         * PhoneInterfaceManager.isRadioOnForSubscriberWithFeature: checkCallingOrSelfReadPhoneState
         * (SecurityException without READ_PHONE_STATE, false with its app op revoked), then
         * the phone for the subscription (getPhone: none for an invalid ID, the default phone
         * for a valid ID without a slot) and its service state not POWER_OFF. Windows' radio
         * switches (airplane mode, the cellular switch, a hardware switch) power the radio
         * off. Without a phone service: the manager's false.
         */
        @Override
        public boolean isRadioOnForSubscriberWithFeature(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "isRadioOnForSubscriber")) return false;
            if (!mCellular.phone(mCellular.phoneId(subId))) return false;
            return WindowsCellular.serviceState(mCellular.modem()).getState() != ServiceState.STATE_POWER_OFF;
        }

        /** isRadioOnForSubscriberWithFeature for the default subscription. */
        @Override
        public boolean isRadioOnWithFeature(String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return false;
            return isRadioOnForSubscriberWithFeature(mCellular.subId(), callingPackage, callingFeatureId);
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

        /**
         * Always answered. Without a modem, TelephonyManager.requestCellInfoUpdate's
         * null-service path: from target S (NULL_TELEPHONY_THROW_NO_CB) IllegalStateException,
         * before that a silent return. With one, see cellInfoUpdate. (Default accepted the
         * request and never called back.)
         */
        @Override
        public void requestCellInfoUpdate(int subId, ICellInfoCallback cb, String callingPackage,
                String callingFeatureId) {
            cellInfoUpdate(cb, callingPackage, callingFeatureId);
        }

        @Override
        public void requestCellInfoUpdateWithWorkSource(int subId, ICellInfoCallback cb,
                String callingPackage, String callingFeatureId, WorkSource workSource) {
            cellInfoUpdate(cb, callingPackage, callingFeatureId);
        }

        /**
         * With a modem: PhoneInterfaceManager.requestCellInfoUpdateInternal. Fine location is
         * required at every target (a soft denial answers an empty list); Windows' Mobile
         * Broadband API reports no cell information, which is the radio failing the
         * request (RIL REQUEST_NOT_SUPPORTED), answered as EVENT_REQUEST_CELL_INFO_UPDATE_DONE
         * answers an exception: CellInfoCallback.ERROR_MODEM_ERROR.
         */
        private void cellInfoUpdate(ICellInfoCallback cb, String callingPackage,
                String callingFeatureId) {
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mCellular.present()) {
                if (c.targetSdk() > Build.VERSION_CODES.R) throw new IllegalStateException("Telephony is null");
                return;
            }
            Location location = mRules.location(c, Build.VERSION_CODES.BASE, Build.VERSION_CODES.BASE);
            if (location == Location.DENIED_HARD) throw new SecurityException("Not allowed to access cell info");
            if (cb == null) return;
            try {
                if (location == Location.DENIED_SOFT) {
                    cb.onCellInfo(new ArrayList<CellInfo>());
                } else {
                    cb.onError(TelephonyManager.CellInfoCallback.ERROR_MODEM_ERROR,
                            CELL_INFO_UNSUPPORTED, CELL_INFO_UNSUPPORTED + ": REQUEST_NOT_SUPPORTED");
                }
            } catch (RemoteException e) {
                // Dropped, as PhoneInterfaceManager drops it.
            }
        }

        /** The exception class AOSP reports when the radio does not support a request. */
        static final String CELL_INFO_UNSUPPORTED = "com.android.internal.telephony.CommandException";

        /**
         * PhoneInterfaceManager.getAllCellInfo: coarse location at every target, fine from Q;
         * a soft denial and a radio that reports no cells both answer an empty list
         * (EVENT_GET_ALL_CELL_INFO_DONE replaces a failed request's result with one).
         * Without a phone, null: the manager's null-service answer.
         */
        @Override
        public List<CellInfo> getAllCellInfo(String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (mRules.location(c, Build.VERSION_CODES.Q, Build.VERSION_CODES.BASE) == Location.DENIED_HARD) {
                throw new SecurityException("Not allowed to access cell info");
            }
            return new ArrayList<>();
        }

        /**
         * PhoneInterfaceManager.getCellLocation. No cell identity is reported, so the answer
         * is the empty CellIdentityGsm AOSP gives a soft denial, which TelephonyManager
         * turns into a null CellLocation. Default's null would make TelephonyManager throw
         * NullPointerException; without a phone the empty identity gives the manager's
         * null-service answer (null) as well.
         */
        @Override
        public CellIdentity getCellLocation(String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return new CellIdentityGsm();
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (mRules.location(c, Build.VERSION_CODES.Q, Build.VERSION_CODES.BASE) == Location.DENIED_HARD) {
                throw new SecurityException("Not allowed to access cell location");
            }
            return new CellIdentityGsm();
        }

        // ---- Data switches (docs/telephony.md, "Mobile data switches"). ----

        /**
         * PhoneInterfaceManager's phone for a subscription (getPhone / getPhoneId): phone 0
         * for the active subscription or a valid ID without a slot, none for an invalid ID
         * (DEFAULT_SUBSCRIPTION_ID without a SIM). Without a phone the switches read false
         * and setters change nothing, as there.
         */
        private boolean dataPhone(int subId) {
            return mCellular.phone(mCellular.phoneId(subId));
        }

        private void requireAny(Caller c, String method, String... permissions) {
            if (!mRules.anyGranted(c, permissions)) {
                throw new SecurityException(method + ": uid " + c.uid() + " has none of "
                        + String.join(", ", permissions));
            }
        }

        /** PhoneInterfaceManager.enforceModifyPermission (Context.enforceCallingOrSelfPermission). */
        private void enforceModify(Caller c) {
            if (!mRules.granted(c, MODIFY_PHONE_STATE)) {
                throw new SecurityException("Neither user " + c.uid() + " nor current process has "
                        + MODIFY_PHONE_STATE + ".");
            }
        }

        /** DataSettingsManager.isDataEnabled: every switch (user, policy, carrier, thermal). */
        @Override
        public boolean isDataEnabled(int subId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(null, null);
            requireAny(c, "isDataEnabled", ACCESS_NETWORK_STATE, READ_PHONE_STATE,
                    READ_BASIC_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE);
            return dataPhone(subId) && mData.enabled();
        }

        @Override
        public boolean isDataEnabledForReason(int subId, int reason) {
            // TelephonyManager.isDataEnabledForReason throws this without the service.
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            Caller c = mRules.caller(null, null);
            requireAny(c, "isDataEnabledForReason", ACCESS_NETWORK_STATE, READ_BASIC_PHONE_STATE,
                    READ_PHONE_STATE, MODIFY_PHONE_STATE);
            return dataPhone(subId) && mData.enabledForReason(reason);
        }

        @Override
        public boolean isUserDataEnabled(int subId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(null, null);
            requireAny(c, "isUserDataEnabled", READ_BASIC_PHONE_STATE, ACCESS_NETWORK_STATE,
                    MODIFY_PHONE_STATE);
            return dataPhone(subId) && mData.userEnabled();
        }

        /** PhoneInterfaceManager.getDataEnabled: isUserDataEnabled. */
        @Override
        public boolean getDataEnabled(int subId) {
            return isUserDataEnabled(subId);
        }

        @Override
        public boolean isDataRoamingEnabled(int subId) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(null, null);
            requireAny(c, "isDataRoamingEnabled", ACCESS_NETWORK_STATE, READ_BASIC_PHONE_STATE,
                    READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE);
            return dataPhone(subId) && mData.roamingEnabled();
        }

        /**
         * PhoneInterfaceManager.setDataEnabledForReason: the user and carrier reasons accept
         * carrier privileges (no carrier apps exist) or MODIFY_PHONE_STATE, the others
         * MODIFY_PHONE_STATE. The user, policy and thermal switches go to DataSettingsManager
         * (the user switch is Settings.Global.MOBILE_DATA); the carrier reason is a carrier
         * action on metered APNs (CarrierActionAgent), which has no APNs to act on here and
         * leaves isDataEnabledForReason(CARRIER) as it is, as on AOSP. Without a phone for
         * the subscription nothing changes. Without the service, the manager's
         * IllegalStateException.
         */
        @Override
        public void setDataEnabledForReason(int subId, int reason, boolean enabled, String callingPackage) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            enforceModify(mRules.caller(null, null));
            if (!dataPhone(subId) || reason == TelephonyManager.DATA_ENABLED_REASON_CARRIER) return;
            mData.setEnabled(reason, enabled);
        }

        /**
         * PhoneInterfaceManager.setDataRoamingEnabled: MODIFY_PHONE_STATE or carrier
         * privileges (SecurityException with the method name), then the phone's
         * Settings.Global.DATA_ROAMING. Without the service the manager does nothing.
         */
        @Override
        public void setDataRoamingEnabled(int subId, boolean isEnabled) {
            if (!mCellular.present()) return;
            Caller c = mRules.caller(null, null);
            if (!mRules.granted(c, MODIFY_PHONE_STATE)) throw new SecurityException("setDataRoamingEnabled");
            if (!dataPhone(subId)) return;
            mData.setRoamingEnabled(isEnabled);
        }

        /** The logical modem of slot 0 is enabled (Windows has no per-modem disable). */
        @Override
        public boolean isModemEnabledForSlot(int slotIndex, String callingPackage,
                String callingFeatureId) {
            if (!slot(slotIndex)) return false;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readNonDangerousPhoneState(c)
                    && !mRules.readPhoneState(c, "isModemEnabledForSlot")) {
                throw new SecurityException("Caller has no permission.");
            }
            return true;
        }

        /** No eUICC (FEATURE_TELEPHONY_EUICC is never declared): UNSUPPORTED_CARD_ID. */
        @Override
        public int getCardIdForDefaultEuicc(int subId, String callingPackage) {
            return mCellular.present() ? TelephonyManager.UNSUPPORTED_CARD_ID
                    : TelephonyManager.UNINITIALIZED_CARD_ID;
        }

        /**
         * PhoneInterfaceManager.getUiccCardsInfo: READ_PRIVILEGED_PHONE_STATE gets the
         * cards; with macro_based_opportunistic_networks (on in bp4a) READ_BASIC_PHONE_STATE
         * gets sanitized copies; anyone else a SecurityException. One removable, non-eUICC
         * card in slot 0 while the modem reports one.
         */
        @Override
        public List<UiccCardInfo> getUiccCardsInfo(String callingPackage) {
            if (!mCellular.present()) return new ArrayList<>();
            Caller c = mRules.caller(callingPackage, null);
            boolean privileged = mRules.granted(c, READ_PRIVILEGED_PHONE_STATE);
            if (!privileged && !mRules.readNonDangerousPhoneState(c)) {
                throw new SecurityException("Caller does not have permission.");
            }
            List<UiccCardInfo> cards = new ArrayList<>();
            WindowsCellular.Modem modem = mCellular.modem();
            if (WindowsCellular.cardPresent(modem)) {
                String iccId = WindowsCellular.iccId(modem);
                UiccCardInfo card = new UiccCardInfo(false, 0, null, 0, true, false,
                        List.of(new UiccPortInfo(iccId, 0, 0, modem.simReady())));
                cards.add(privileged ? card : card.createSensitiveInfoSanitizedCopy(false));
            }
            return cards;
        }

        /**
         * USSD needs CALL_PHONE; an invalid subscription returns silently; otherwise the
         * request fails as PhoneInterfaceManager fails it when USSD APIs are not supported:
         * USSD_RETURN_FAILURE with a UssdResponse carrying the request. Without a phone the
         * manager drops the request silently (Default).
         */
        @Override
        public void handleUssdRequest(int subId, String ussdRequest, ResultReceiver wrappedCallback) {
            if (!mCellular.present()) return;
            requireAny(mRules.caller(null, null), "handleUssdRequest", CALL_PHONE);
            if (!SubscriptionManager.isValidSubscriptionId(subId) || wrappedCallback == null) return;
            Bundle data = new Bundle();
            data.putParcelable(TelephonyManager.USSD_RESPONSE, new UssdResponse(ussdRequest, null));
            wrappedCallback.send(TelephonyManager.USSD_RETURN_FAILURE, data);
        }

        /**
         * Only allow-listed carrier apps may ask (CarrierAllowListInfo); Gesher has no
         * allow list, so every caller with the permission is "Not an authorized caller".
         * Without a phone the manager makes no call and no callback (Default).
         */
        @Override
        public void getCarrierRestrictionStatus(IIntegerConsumer callback, String packageName) {
            if (!mCellular.present()) return;
            Caller c = mRules.caller(packageName, null);
            requireAny(c, "getCarrierRestrictionStatus", READ_BASIC_PHONE_STATE, READ_PHONE_STATE);
            throw new SecurityException("Not an authorized caller");
        }

        /** Only the dialer role holder may upload; Gesher has no dialer role holder. */
        @Override
        public void uploadCallComposerPicture(int subscriptionId, String callingPackage,
                String contentType, ParcelFileDescriptor fd, ResultReceiver callback) {
            if (!mCellular.present()) throw new IllegalStateException(NO_TELEPHONY_SERVICE);
            mRules.caller(callingPackage, null);
            throw new SecurityException("App must be the dialer role holder to upload a call composer pic");
        }

        /**
         * Premium capabilities (network slicing upsell): READ_BASIC_PHONE_STATE and INTERNET,
         * then SlicePurchaseController's first answer for a device without slicing upsell,
         * FEATURE_NOT_SUPPORTED. Without a phone, the manager's REQUEST_FAILED.
         */
        @Override
        public void purchasePremiumCapability(int capability, IIntegerConsumer callback, int subId) {
            int result = TelephonyManager.PURCHASE_PREMIUM_CAPABILITY_RESULT_REQUEST_FAILED;
            if (mCellular.present()) {
                Caller c = mRules.caller(null, null);
                if (!mRules.readNonDangerousPhoneState(c)) {
                    throw new SecurityException("purchasePremiumCapability requires permission "
                            + "READ_BASIC_PHONE_STATE.");
                }
                if (!mRules.granted(c, INTERNET)) {
                    throw new SecurityException("purchasePremiumCapability requires permission INTERNET.");
                }
                if (mCellular.phoneForSub(subId) == 0) {
                    result = TelephonyManager.PURCHASE_PREMIUM_CAPABILITY_RESULT_FEATURE_NOT_SUPPORTED;
                }
            }
            try {
                if (callback != null) callback.accept(result);
            } catch (RemoteException e) {
                // The caller is gone.
            }
        }

        @Override
        public boolean isPremiumCapabilityAvailableForPurchase(int capability, int subId) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            if (!mRules.readNonDangerousPhoneState(mRules.caller(null, null))) {
                throw new SecurityException("isPremiumCapabilityAvailableForPurchase requires "
                        + "permission READ_BASIC_PHONE_STATE.");
            }
            return false;
        }

        /**
         * Network slicing configuration needs READ_PRIVILEGED_PHONE_STATE (or carrier
         * privileges). The modem reports none: the request fails with
         * NetworkSlicingException.ERROR_MODEM_ERROR. Without a phone the manager throws.
         */
        @Override
        public void getSlicingConfig(ResultReceiver callback) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            requireAny(mRules.caller(null, null), "getSlicingConfig", READ_PRIVILEGED_PHONE_STATE);
            if (callback != null) {
                callback.send(TelephonyManager.NetworkSlicingException.ERROR_MODEM_ERROR, null);
            }
        }

        /**
         * GBA needs PERFORM_IMS_SINGLE_REGISTRATION or MODIFY_PHONE_STATE; Gesher has no GBA
         * service, so permitted requests fail with FEATURE_NOT_SUPPORTED. Without a phone,
         * the manager's FEATURE_NOT_READY.
         */
        @Override
        public void bootstrapAuthenticationRequest(int subId, int appType, Uri nafUrl,
                UaSecurityProtocolIdentifier securityProtocol, boolean forceBootStrapping,
                IBootstrapAuthenticationCallback callback) {
            int reason = TelephonyManager.GBA_FAILURE_REASON_FEATURE_NOT_READY;
            if (mCellular.present()) {
                requireAny(mRules.caller(null, null), "bootstrapAuthenticationRequest",
                        PERFORM_IMS_SINGLE_REGISTRATION, MODIFY_PHONE_STATE);
                reason = TelephonyManager.GBA_FAILURE_REASON_FEATURE_NOT_SUPPORTED;
            }
            try {
                if (callback != null) callback.onAuthenticationFailure(0, reason);
            } catch (RemoteException e) {
                // The caller is gone.
            }
        }

        /**
         * Network scans need MODIFY_PHONE_STATE or carrier privileges, which applications
         * cannot hold here. A permitted (system) caller gets INVALID_SCAN_ID, which
         * TelephonyScanManager reports as no scan: Windows cannot scan for Gesher (a
         * scan would change the modem's state). Default's 0 was a valid scan ID that
         * never reported anything.
         */
        @Override
        public int requestNetworkScan(int subId, boolean renounceFineLocationAccess,
                NetworkScanRequest request, Messenger messenger, IBinder binder,
                String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return TelephonyScanManager.INVALID_SCAN_ID;
            requireAny(mRules.caller(callingPackage, callingFeatureId), "requestNetworkScan",
                    MODIFY_PHONE_STATE);
            return TelephonyScanManager.INVALID_SCAN_ID;
        }

        /**
         * Emergency numbers as EmergencyNumberTracker knows them when the radio reports none
         * (Windows' Mobile Broadband API has no emergency number list) and its emergency
         * number database has no entry (Gesher ships none): the 3GPP TS 22.101 ECC list,
         * "112,911" with a SIM and "112,911,000,08,110,118,119,999" without one, plus, without
         * a SIM, libphonenumber's ShortNumberInfo for the last known network country. Without
         * a phone the managers throw.
         */
        static final String ECC_WITH_SIM = "112,911";
        static final String ECC_WITHOUT_SIM = "112,911,000,08,110,118,119,999";

        private boolean simAbsent() {
            return !SubscriptionManager.isValidSubscriptionId(mCellular.subId());
        }

        private List<String> eccList() {
            return List.of((simAbsent() ? ECC_WITHOUT_SIM : ECC_WITH_SIM).split(","));
        }

        @Override
        public Map getEmergencyNumberList(String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneState(c, "getEmergencyNumberList")) {
                throw new SecurityException("Requires READ_PHONE_STATE permission.");
            }
            List<android.telephony.emergency.EmergencyNumber> numbers = new ArrayList<>();
            for (String number : eccList()) {
                numbers.add(new android.telephony.emergency.EmergencyNumber(number, "", "",
                        android.telephony.emergency.EmergencyNumber.EMERGENCY_SERVICE_CATEGORY_UNSPECIFIED,
                        new ArrayList<String>(), 0,
                        android.telephony.emergency.EmergencyNumber.EMERGENCY_CALL_ROUTING_UNKNOWN));
            }
            Map<Integer, List<android.telephony.emergency.EmergencyNumber>> result = new HashMap<>();
            result.put(mCellular.subId(), numbers);
            return result;
        }

        @Override
        public boolean isEmergencyNumber(String number, boolean exactMatch) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            if (!exactMatch) {
                requireAny(mRules.caller(null, null), "isEmergencyNumber(Potential)",
                        READ_PRIVILEGED_PHONE_STATE);
            }
            if (number == null || android.telephony.PhoneNumberUtils.isUriNumber(number)) return false;
            number = android.telephony.PhoneNumberUtils.extractNetworkPortionAlt(number);
            if (eccList().contains(number)) return true;
            if (!simAbsent()) return false;
            WindowsCellular.Modem modem = mCellular.modem();
            String iso = modem == null ? "" : WindowsCellular.countryIso(
                    WindowsCellular.plmn(modem.registeredProviderId));
            if (iso.isEmpty()) return false;
            return com.android.i18n.phonenumbers.ShortNumberInfo.getInstance()
                    .isEmergencyNumber(number, iso.toUpperCase(java.util.Locale.ROOT));
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

        /**
         * PhoneSubInfoController.callPhoneMethodForPhoneIdWithReadDeviceIdentifiersCheck: a
         * phone ID that is not valid (anything but 0 with one modem) is phone 0; then device
         * identifier access.
         */
        @Override
        public String getDeviceIdForPhone(int phoneId, String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readIdentifiers(c, "getDeviceId")) return null;
            return WindowsCellular.imei(mCellular.modem());
        }

        @Override
        public String getDeviceIdWithFeature(String callingPackage, String callingFeatureId) {
            return getDeviceIdForPhone(mCellular.phoneId(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID),
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

        // PhoneSubInfoController's remaining getters, with its permission helpers: the
        // check first (SecurityException, or null for a revoked app op), then the phone
        // for the subscription (null without one). Windows' Mobile Broadband API reports
        // the subscriber ID, ICCID and telephone numbers only; GID1/GID2, the line 1 alpha
        // tag, the voicemail number and tag, the IMEISV and the NAI are not reported, so
        // they are null after the check. Without a modem: null, the managers' answer.

        /** callPhoneMethodForSubIdWithReadCheck: checkCallingOrSelfReadPhoneState. */
        private boolean readCheck(int subId, String callingPackage, String callingFeatureId,
                String message) {
            if (!mCellular.present()) return false;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            return mRules.readPhoneState(c, message) && modemForSub(subId) != null;
        }

        @Override
        public String getGroupIdLevel1ForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getGroupIdLevel1");
            return null;
        }

        @Override
        public String getGroupIdLevel2ForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getGroupIdLevel2");
            return null;
        }

        @Override
        public String getLine1AlphaTag(String callingPackage, String callingFeatureId) {
            return getLine1AlphaTagForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getLine1AlphaTagForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getLine1AlphaTag");
            return null;
        }

        @Override
        public String getVoiceMailNumber(String callingPackage, String callingFeatureId) {
            return getVoiceMailNumberForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getVoiceMailNumberForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getVoiceMailNumber");
            return null;
        }

        @Override
        public String getVoiceMailAlphaTag(String callingPackage, String callingFeatureId) {
            return getVoiceMailAlphaTagForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getVoiceMailAlphaTagForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getVoiceMailAlphaTag");
            return null;
        }

        @Override
        public String getDeviceSvn(String callingPackage, String callingFeatureId) {
            return getDeviceSvnUsingSubId(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getDeviceSvnUsingSubId(int subId, String callingPackage,
                String callingFeatureId) {
            readCheck(subId, callingPackage, callingFeatureId, "getDeviceSvn");
            return null;
        }

        /** The NAI is a subscriber identifier (checkCallingOrSelfReadSubscriberIdentifiers). */
        @Override
        public String getNaiForSubscriber(int subId, String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) return null;
            mRules.readIdentifiers(mRules.caller(callingPackage, callingFeatureId), "getNai");
            return null;
        }

        /** The MSISDN: checkCallingOrSelfReadPhoneNumber, as for the line 1 number. */
        @Override
        public String getMsisdn(String callingPackage, String callingFeatureId) {
            return getMsisdnForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getMsisdnForSubscriber(int subId, String callingPackage,
                String callingFeatureId) {
            if (!mCellular.present()) return null;
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (!mRules.readPhoneNumber(c, "getMsisdn")) return null;
            return WindowsCellular.number(modemForSub(subId));
        }

        // The deprecated default-subscription overloads.
        @Override
        public String getDeviceId(String callingPackage) {
            return getDeviceIdWithFeature(callingPackage, null);
        }

        @Override
        public String getSubscriberId(String callingPackage) {
            return getSubscriberIdWithFeature(callingPackage, null);
        }

        @Override
        public String getSubscriberIdWithFeature(String callingPackage, String callingFeatureId) {
            return getSubscriberIdForSubscriber(SubscriptionManager.DEFAULT_SUBSCRIPTION_ID,
                    callingPackage, callingFeatureId);
        }

        @Override
        public String getIccSerialNumber(String callingPackage) {
            return getIccSerialNumberWithFeature(callingPackage, null);
        }
    }

    // ---------------------------------------------------------------------------------
    // isub: ISub.
    // ---------------------------------------------------------------------------------

    static final class Subscriptions extends ISub.Default {
        private final Cellular mCellular;
        private final Rules mRules;
        // The subscription database's data roaming column (ISub.setDataRoaming), per
        // subscription ID; like the IDs themselves it lasts for the boot session.
        private final Map<Integer, Integer> mDataRoaming = new java.util.concurrent.ConcurrentHashMap<>();
        // TelephonyRegistry.notifySubscriptionInfoChanged, when a record changes.
        private volatile Runnable mRecordChanged = () -> { };

        Subscriptions(Cellular cellular, Rules rules) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
        }

        void onRecordChanged(Runnable listener) { mRecordChanged = Objects.requireNonNull(listener); }

        /**
         * SubscriptionManagerService.setDataRoaming: MODIFY_PHONE_STATE, a non-negative
         * mode, a subscription that exists (SubscriptionDatabaseManager throws
         * IllegalArgumentException otherwise); a changed column notifies subscription
         * listeners. This is the record's SubscriptionInfo.getDataRoaming(), which is not the
         * DATA_ROAMING switch (TelephonyManager.setDataRoamingEnabled), as on AOSP. Without
         * the service, SubscriptionManager's answer: 0 records.
         */
        @Override
        public int setDataRoaming(int roaming, int subId) {
            if (!mCellular.present()) return 0;
            Caller c = mRules.caller(null, null);
            if (!mRules.granted(c, MODIFY_PHONE_STATE)) {
                throw new SecurityException("setDataRoaming. Does not have any of the following permissions. ["
                        + MODIFY_PHONE_STATE + "]");
            }
            if (roaming < 0) throw new IllegalArgumentException("Invalid roaming value " + roaming);
            int active = mCellular.subId();
            if (!SubscriptionManager.isValidSubscriptionId(subId) || subId != active) {
                throw new IllegalArgumentException("Subscription doesn't exist. subId=" + subId
                        + ", columnName=data_roaming");
            }
            Integer old = mDataRoaming.put(subId, roaming);
            if ((old == null ? SubscriptionManager.DATA_ROAMING_DISABLE : old) != roaming) mRecordChanged.run();
            return 1;
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
                    .setUiccApplicationsEnabled(true)
                    .setDataRoaming(mDataRoaming.getOrDefault(subId, SubscriptionManager.DATA_ROAMING_DISABLE));
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
            // AOSP retains the default-phone sentinel for a valid but unmapped ID.
            return mCellular.phoneId(subId);
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

        /**
         * No subscription belongs to a group. From target T reading a group needs device
         * identifier access (REQUIRE_DEVICE_IDENTIFIERS_FOR_GROUP_UUID). Without isub,
         * SubscriptionManager throws IllegalStateException to applications.
         */
        @Override
        public List<SubscriptionInfo> getSubscriptionsInGroup(android.os.ParcelUuid groupUuid,
                String callingPackage, String callingFeatureId) {
            if (!mCellular.present()) throw new IllegalStateException(TELEPHONY_SERVICE_NULL);
            Caller c = mRules.caller(callingPackage, callingFeatureId);
            if (c.targetSdk() >= Build.VERSION_CODES.TIRAMISU
                    && !mRules.readIdentifiers(c, "getSubscriptionsInGroup")) {
                throw new SecurityException("Need to have carrier privileges or access to "
                        + "device identifiers to call getSubscriptionsInGroup");
            }
            return Collections.emptyList();
        }

        /** No opportunistic subscriptions (the manager also turns null into an empty list). */
        @Override
        public List<SubscriptionInfo> getOpportunisticSubscriptions(String callingPackage,
                String callingFeatureId) {
            return Collections.emptyList();
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
        private final DataSettings mData;
        private final Object mLock = new Object();
        private final Map<IBinder, Record> mRecords = new HashMap<>();
        private final List<IOnSubscriptionsChangedListener> mSubscriptionListeners = new ArrayList<>();
        // A death recipient per remote listener binder, so a dead app's records are dropped
        // as TelephonyRegistry drops them, even when no event is ever delivered (no modem).
        private final Map<IBinder, IBinder.DeathRecipient> mDeathRecipients = new HashMap<>();
        // The values last delivered, so listeners only hear real changes.
        private ServiceState mServiceState;
        private WindowsCellular.Modem mModem;
        private SignalStrength mSignal;
        private int mDataState = -1, mDataType = -1, mSubId;
        private TelephonyDisplayInfo mDisplayInfo;
        // The user switch and every switch (TelephonyRegistry's mUserMobileDataState and
        // mIsDataEnabled), with the reason of the last change, for the active subscription.
        private boolean mUserDataEnabled, mDataEnabled;
        private int mDataEnabledReason = TelephonyManager.DATA_ENABLED_REASON_USER;

        private static final class Record {
            IPhoneStateListener callback;
            Caller caller;
            Set<Integer> events;
            int subId;
            boolean renounceFine, renounceCoarse;
        }

        Registry(Cellular cellular, Rules rules, DataSettings data) {
            mCellular = Objects.requireNonNull(cellular);
            mRules = Objects.requireNonNull(rules);
            mData = Objects.requireNonNull(data);
            mSubId = cellular.readingSubId();
            mModem = cellular.reading();
            mServiceState = cellular.present() ? WindowsCellular.serviceState(mModem) : null;
            mSignal = cellular.present() ? signal() : null;
            mDataState = WindowsCellular.dataState(mModem);
            mDataType = WindowsCellular.dataNetworkType(mModem);
            mDisplayInfo = WindowsCellular.displayInfo(mModem);
            mUserDataEnabled = userDataEnabled(mSubId);
            mDataEnabled = dataEnabled(mSubId);
            cellular.addListener(this::onModemChanged);
            data.addListener(this::onDataSettingsChanged);
        }

        /** The user switch as the phone of an active subscription reports it; false without one. */
        private boolean userDataEnabled(int subId) {
            return mCellular.present() && SubscriptionManager.isValidSubscriptionId(subId) && mData.userEnabled();
        }

        private boolean dataEnabled(int subId) {
            return mCellular.present() && SubscriptionManager.isValidSubscriptionId(subId) && mData.enabled();
        }

        /**
         * DataSettingsManager's notifications of a switch change: the user switch
         * (notifyUserMobileDataStateChanged) when it changed, and every switch
         * (notifyDataEnabled, with the change's reason) when the overall answer changed.
         */
        private void onDataSettingsChanged(int reason) {
            synchronized (mLock) {
                boolean user = userDataEnabled(mSubId);
                boolean enabled = dataEnabled(mSubId);
                boolean userChanged = user != mUserDataEnabled;
                boolean enabledChanged = enabled != mDataEnabled;
                mUserDataEnabled = user;
                mDataEnabled = enabled;
                if (enabledChanged) mDataEnabledReason = reason;
                for (Record r : new ArrayList<>(mRecords.values())) {
                    if (r.subId != SubscriptionManager.DEFAULT_SUBSCRIPTION_ID && r.subId != mSubId) continue;
                    try {
                        if (userChanged && r.events.contains(TelephonyCallback.EVENT_USER_MOBILE_DATA_STATE_CHANGED)) {
                            r.callback.onUserMobileDataStateChanged(user);
                        }
                        if (enabledChanged && r.events.contains(TelephonyCallback.EVENT_DATA_ENABLED_CHANGED)) {
                            r.callback.onDataEnabledChanged(enabled, reason);
                        }
                    } catch (RemoteException | RuntimeException e) {
                        mRecords.remove(r.callback.asBinder());
                        watchLocked();
                    }
                }
            }
        }

        /**
         * TelephonyRegistry.notifySubscriptionInfoChanged: a subscription record changed
         * (ISub.setDataRoaming), so subscription listeners are called.
         */
        void subscriptionRecordChanged() {
            List<IOnSubscriptionsChangedListener> listeners;
            synchronized (mLock) {
                listeners = new ArrayList<>(mSubscriptionListeners);
            }
            for (IOnSubscriptionsChangedListener listener : listeners) {
                try {
                    listener.onSubscriptionsChanged();
                } catch (RemoteException | RuntimeException e) {
                    removeSubscriptionListener(listener);
                }
            }
        }

        private SignalStrength signal() {
            WindowsCellular.Modem modem = mCellular.reading();
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
            IBinder binder = callback.asBinder();
            synchronized (mLock) {
                // One record per listener binder, as TelephonyRegistry keeps.
                mSubscriptionListeners.removeIf(l -> l.asBinder() == binder);
                mSubscriptionListeners.add(callback);
                watchLocked();
            }
            if (!watchDeath(binder)) {
                removeSubscriptionListener(callback);
                return;
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
            IBinder binder = callback.asBinder();
            synchronized (mLock) {
                mSubscriptionListeners.removeIf(l -> l.asBinder() == binder);
                watchLocked();
            }
            unwatchIfUnused(binder);
        }

        /**
         * Drops every record of a listener binder when its process dies. In-process
         * (local) binders cannot die separately. Returns false if the binder is already dead.
         */
        private boolean watchDeath(IBinder binder) {
            if (binder == null || binder instanceof Binder) return true;
            IBinder.DeathRecipient recipient;
            synchronized (mLock) {
                if (mDeathRecipients.containsKey(binder)) return true;
                recipient = () -> binderDied(binder);
                mDeathRecipients.put(binder, recipient);
            }
            try {
                binder.linkToDeath(recipient, 0);
                return true;
            } catch (RemoteException e) {
                synchronized (mLock) {
                    mDeathRecipients.remove(binder);
                }
                return false;
            }
        }

        private void binderDied(IBinder binder) {
            synchronized (mLock) {
                mRecords.remove(binder);
                mSubscriptionListeners.removeIf(l -> l.asBinder() == binder);
                mDeathRecipients.remove(binder);
                watchLocked();
            }
        }

        /** Unlinks a binder's death recipient once no record of it remains. */
        private void unwatchIfUnused(IBinder binder) {
            IBinder.DeathRecipient recipient;
            synchronized (mLock) {
                if (mRecords.containsKey(binder)) return;
                for (IOnSubscriptionsChangedListener l : mSubscriptionListeners) {
                    if (l.asBinder() == binder) return;
                }
                recipient = mDeathRecipients.remove(binder);
            }
            if (recipient == null) return;
            try {
                binder.unlinkToDeath(recipient, 0);
            } catch (RuntimeException e) {
                // Already dead or never linked: nothing remains to unlink.
            }
        }

        /** Tells the modem model whether listeners exist (it polls while they do). Called with mLock. */
        private void watchLocked() {
            mCellular.setWatched(!mRecords.isEmpty() || !mSubscriptionListeners.isEmpty());
        }

        /** Live listener records (phone-state and subscription), for tests. */
        int listenerCount() {
            synchronized (mLock) {
                return mRecords.size() + mSubscriptionListeners.size();
            }
        }

        /**
         * checkListenerPermission: SecurityException, false (not registered: a revoked app
         * op or a soft location denial) or true. Cell location and cell info use
         * LocationAccessPolicy with fine location from Q and coarse location at every
         * target: a hard denial throws, a soft one (location switch off, app op ignored)
         * registers nothing.
         */
        private boolean permitted(Set<Integer> events, Caller c) {
            boolean ok = true;
            if (events.contains(TelephonyCallback.EVENT_CELL_LOCATION_CHANGED)
                    || events.contains(TelephonyCallback.EVENT_CELL_INFO_CHANGED)) {
                Location location = mRules.location(c, Build.VERSION_CODES.Q, Build.VERSION_CODES.BASE);
                if (location == Location.DENIED_HARD) {
                    throw new SecurityException("Unable to listen for events " + events + " due to "
                            + "insufficient location permissions.");
                }
                if (location == Location.DENIED_SOFT) ok = false;
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
                synchronized (mLock) {
                    mRecords.remove(binder);
                    watchLocked();
                }
                unwatchIfUnused(binder);
                return;
            }
            if (!permitted(eventSet, c)) return;
            if (!watchDeath(binder)) return;
            Record r = new Record();
            r.callback = callback;
            r.caller = c;
            r.events = eventSet;
            r.subId = SubscriptionManager.isValidSubscriptionId(subId) ? subId
                    : SubscriptionManager.DEFAULT_SUBSCRIPTION_ID;
            r.renounceFine = renounceFine;
            r.renounceCoarse = renounceCoarse;
            // A query (it may read the modem), made before taking mLock.
            boolean phone = mCellular.phoneForSub(r.subId) == 0;
            synchronized (mLock) {
                mRecords.put(binder, r);
                if (notifyNow && phone) {
                    initial(r);
                }
                watchLocked();
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
                // TelephonyRegistry's initial values for a phone: no data activity is
                // reported (DATA_ACTIVITY_NONE, as getDataActivity answers), no cell identity
                // or cell info (with the same location checks), the display info, the
                // active data subscription and the data switches (dataEnabled).
                if (r.events.contains(TelephonyCallback.EVENT_DATA_ACTIVITY_CHANGED)) {
                    r.callback.onDataActivity(TelephonyManager.DATA_ACTIVITY_NONE);
                }
                if (r.events.contains(TelephonyCallback.EVENT_CELL_LOCATION_CHANGED)
                        && coarse(r, Build.VERSION_CODES.BASE) && fine(r, Build.VERSION_CODES.Q)) {
                    r.callback.onCellLocationChanged(null);
                }
                if (r.events.contains(TelephonyCallback.EVENT_CELL_INFO_CHANGED)
                        && coarse(r, Build.VERSION_CODES.BASE) && fine(r, Build.VERSION_CODES.Q)) {
                    r.callback.onCellInfoChanged(new ArrayList<>());
                }
                if (r.events.contains(TelephonyCallback.EVENT_DISPLAY_INFO_CHANGED)) {
                    r.callback.onDisplayInfoChanged(mDisplayInfo);
                }
                if (r.events.contains(TelephonyCallback.EVENT_ACTIVE_DATA_SUBSCRIPTION_ID_CHANGED)) {
                    r.callback.onActiveDataSubIdChanged(mSubId);
                }
                if (r.events.contains(TelephonyCallback.EVENT_USER_MOBILE_DATA_STATE_CHANGED)) {
                    r.callback.onUserMobileDataStateChanged(mUserDataEnabled);
                }
                if (r.events.contains(TelephonyCallback.EVENT_DATA_ENABLED_CHANGED)) {
                    r.callback.onDataEnabledChanged(mDataEnabled, mDataEnabledReason);
                }
            } catch (RemoteException | RuntimeException e) {
                mRecords.remove(r.callback.asBinder());
                watchLocked();
            }
        }

        /** TelephonyRegistry.checkFineLocationAccess. */
        private boolean fine(Record r, int minSdk) {
            return !r.renounceFine && mRules.location(r.caller, minSdk, minSdk) == Location.ALLOWED;
        }

        /** TelephonyRegistry.checkCoarseLocationAccess. */
        private boolean coarse(Record r, int minSdk) {
            return !r.renounceCoarse
                    && mRules.location(r.caller, Integer.MAX_VALUE, minSdk) == Location.ALLOWED;
        }

        /** checkFineLocationAccess(r, Q) and checkCoarseLocationAccess(r, Q). */
        private ServiceState serviceStateFor(Record r) {
            return forCaller(mModem, fine(r, Build.VERSION_CODES.Q), coarse(r, Build.VERSION_CODES.Q));
        }

        private void onModemChanged() {
            List<IOnSubscriptionsChangedListener> subscriptionListeners;
            synchronized (mLock) {
                WindowsCellular.Modem modem = mCellular.reading();
                ServiceState state = WindowsCellular.serviceState(modem);
                SignalStrength signal = modem == null ? new SignalStrength()
                        : WindowsCellular.signalStrength(modem);
                int dataState = WindowsCellular.dataState(modem);
                int dataType = WindowsCellular.dataNetworkType(modem);
                boolean stateChanged = !state.equals(mServiceState);
                boolean signalChanged = !signal.equals(mSignal);
                boolean dataChanged = dataState != mDataState || dataType != mDataType;
                int subId = mCellular.readingSubId();
                boolean subscriptionsChanged = subId != mSubId;
                TelephonyDisplayInfo displayInfo = WindowsCellular.displayInfo(modem);
                boolean displayChanged = !displayInfo.equals(mDisplayInfo);
                // A subscription change re-reads the switches (DataSettingsManager's
                // EVENT_SUBSCRIPTIONS_CHANGED, reason USER).
                boolean userDataEnabled = userDataEnabled(subId);
                boolean userDataChanged = userDataEnabled != mUserDataEnabled;
                boolean dataEnabled = dataEnabled(subId);
                boolean dataEnabledChanged = dataEnabled != mDataEnabled;
                mDisplayInfo = displayInfo;
                mUserDataEnabled = userDataEnabled;
                mDataEnabled = dataEnabled;
                if (dataEnabledChanged) mDataEnabledReason = TelephonyManager.DATA_ENABLED_REASON_USER;
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
                        if (displayChanged && r.events.contains(TelephonyCallback.EVENT_DISPLAY_INFO_CHANGED)) {
                            r.callback.onDisplayInfoChanged(displayInfo);
                        }
                        if (subscriptionsChanged
                                && r.events.contains(TelephonyCallback.EVENT_ACTIVE_DATA_SUBSCRIPTION_ID_CHANGED)) {
                            r.callback.onActiveDataSubIdChanged(subId);
                        }
                        if (userDataChanged
                                && r.events.contains(TelephonyCallback.EVENT_USER_MOBILE_DATA_STATE_CHANGED)) {
                            r.callback.onUserMobileDataStateChanged(userDataEnabled);
                        }
                        if (dataEnabledChanged && r.events.contains(TelephonyCallback.EVENT_DATA_ENABLED_CHANGED)) {
                            r.callback.onDataEnabledChanged(dataEnabled, TelephonyManager.DATA_ENABLED_REASON_USER);
                        }
                    } catch (RemoteException | RuntimeException e) {
                        mRecords.remove(r.callback.asBinder());
                        watchLocked();
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
            this(cellular, callers, memorySettings());
        }

        Endpoints(Cellular cellular, Callers callers, GlobalSettings settings) {
            Rules rules = new Rules(callers);
            DataSettings data = new DataSettings(settings);
            this.cellular = cellular;
            phone = new Phone(cellular, rules, data);
            phoneSubInfo = new PhoneSubInfo(cellular, rules);
            subscriptions = new Subscriptions(cellular, rules);
            registry = new Registry(cellular, rules, data);
            subscriptions.onRecordChanged(registry::subscriptionRecordChanged);
        }
    }

    /** Production services, over the boot snapshot of Windows' Mobile Broadband service. */
    static Endpoints create(PackageManagerService pm, ActivityManagerService am,
            BooleanSupplier locationEnabled) {
        Cellular cellular = new Cellular(WindowsCellular.WINDOWS, bootSnapshot(), SYSTEM_PROPERTIES);
        // Polling starts with the first query or listener (Cellular).
        return new Endpoints(cellular, callers(pm, am, locationEnabled), SETTINGS_PROVIDER);
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
