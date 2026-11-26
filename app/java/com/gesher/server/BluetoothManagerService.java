/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothHeadset;
import android.bluetooth.BluetoothProfile;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelUuid;
import android.os.Parcelable;
import android.os.RemoteException;
import android.os.ServiceManager;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * {@code gesher.bluetooth}: the part of Android's Bluetooth stack that belongs to the device
 * rather than to an app (docs/bluetooth.md). It watches Windows' radio and paired devices
 * and sends the system's Bluetooth broadcasts ({@code ACTION_STATE_CHANGED},
 * {@code ACTION_ACL_CONNECTED}/{@code DISCONNECTED}, {@code ACTION_BOND_STATE_CHANGED}, the
 * A2DP and headset profiles' {@code ACTION_CONNECTION_STATE_CHANGED}), runs classic discovery
 * ({@code ACTION_DISCOVERY_STARTED}, {@code ACTION_FOUND}, {@code ACTION_DISCOVERY_FINISHED})
 * and SDP refreshes ({@code ACTION_UUID}), and turns the radio on and off.
 *
 * <p>Scanning, GATT and sockets run in the app's own process, straight on
 * {@link WindowsBluetooth} (Windows arbitrates the radio, as for cameras): only what Android
 * broadcasts as the system needs the system server. A plain Binder with its own transaction
 * codes, called by {@link Client}; the watch starts with the first call, so nothing polls
 * while no app uses Bluetooth.
 */
public final class BluetoothManagerService extends Binder {
    public static final String NAME = "gesher.bluetooth";
    public static final String DESCRIPTOR = "com.gesher.IBluetooth";

    static final int STATE = IBinder.FIRST_CALL_TRANSACTION;
    static final int SET_ENABLED = IBinder.FIRST_CALL_TRANSACTION + 1;
    static final int START_DISCOVERY = IBinder.FIRST_CALL_TRANSACTION + 2;
    static final int CANCEL_DISCOVERY = IBinder.FIRST_CALL_TRANSACTION + 3;
    static final int IS_DISCOVERING = IBinder.FIRST_CALL_TRANSACTION + 4;
    static final int FETCH_UUIDS = IBinder.FIRST_CALL_TRANSACTION + 5;
    static final int CREATE_BOND = IBinder.FIRST_CALL_TRANSACTION + 6;
    static final int REMOVE_BOND = IBinder.FIRST_CALL_TRANSACTION + 7;

    static final UUID AUDIO_SINK = UUID.fromString("0000110b-0000-1000-8000-00805f9b34fb");
    static final UUID ADVANCED_AUDIO = UUID.fromString("0000110d-0000-1000-8000-00805f9b34fb");
    static final UUID HANDSFREE = UUID.fromString("0000111e-0000-1000-8000-00805f9b34fb");
    static final UUID HEADSET = UUID.fromString("00001108-0000-1000-8000-00805f9b34fb");

    /** How long a discovery runs (Android's is about 12 s). */
    static final int DISCOVERY_SECONDS = 10;

    /** Where the broadcasts go; the system server's ActivityManagerService. */
    public interface Broadcaster {
        void send(Intent intent);
    }

    /**
     * Who is calling, as the Bluetooth module's service checks it: the caller's package
     * (null for the system), its target SDK, its permissions, and the user's consent to
     * switching the PC's radio.
     */
    public interface Policy {
        /** The calling package, or null when the system calls. */
        String callerPackage();

        int targetSdk(String packageName);

        boolean granted(String packageName, String permission);

        /** Asks the user (Gesher's consent dialog) whether {@code packageName} may do it. */
        boolean confirm(String packageName, String what);
    }

    /** Callers' identity and grants from the system server's services. */
    static final class ServerPolicy implements Policy {
        private final CallerIdentity mCallers;
        private final PackageManagerService mPm;

        ServerPolicy(CallerIdentity callers, PackageManagerService pm) {
            mCallers = callers;
            mPm = pm;
        }

        @Override
        public String callerPackage() {
            return mCallers.isSystem() ? null : mCallers.callerPackage();
        }

        @Override
        public int targetSdk(String packageName) {
            ApkPackage p = mPm.get(packageName);
            return p == null ? 0 : p.parsed.getTargetSdkVersion();
        }

        @Override
        public boolean granted(String packageName, String permission) {
            return mPm.permissions().checkPermission(packageName, permission)
                    == PackageManager.PERMISSION_GRANTED;
        }

        @Override
        public boolean confirm(String packageName, String what) {
            return PermissionRequests.confirm(packageName, what);
        }
    }

    private final Broadcaster mBroadcaster;
    private final Policy mPolicy;
    private final Object mLock = new Object();
    private final Object mPollLock = new Object();
    private Thread mWatch;
    private Thread mDiscovery;
    private volatile boolean mDiscovering;
    private int mState = -1;  // BluetoothAdapter.STATE_*, -1 before the first look
    private Map<Long, WindowsBluetooth.Device> mDevices = new HashMap<>();
    /** Longest wait between looks: Windows' events (radio, connections) wake the watch earlier. */
    private long mPollMs = 10000;
    private final java.util.Set<Long> mPairing = new java.util.HashSet<>();

    BluetoothManagerService(ActivityManagerService am, PackageManagerService pm) {
        this(intent -> am.sendBroadcastFor("android", intent, null),
                am == null || pm == null ? null : new ServerPolicy(am.callers(), pm));
    }

    /** For tests: broadcasts go to {@code broadcaster}; a null policy trusts every caller. */
    public BluetoothManagerService(Broadcaster broadcaster, Policy policy) {
        super(DESCRIPTOR);
        mBroadcaster = broadcaster;
        mPolicy = policy;
    }

    /** For tests: how often the watch looks at Windows. */
    public void setPollInterval(long ms) {
        mPollMs = ms;
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code < STATE || code > REMOVE_BOND) return super.onTransact(code, data, reply, flags);
        data.enforceInterface(DESCRIPTOR);
        startWatch();
        try {
            switch (code) {
                case STATE: {
                    int state = state();
                    reply.writeNoException();
                    reply.writeInt(state);
                    reply.writeBoolean(mDiscovering);
                    return true;
                }
                case SET_ENABLED: {
                    boolean on = data.readBoolean();
                    boolean ok = mayToggle(on) && setEnabled(on);
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                case START_DISCOVERY: {
                    enforceScan();
                    boolean ok = startDiscovery();
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                case CANCEL_DISCOVERY: {
                    enforceScan();
                    boolean ok = cancelDiscovery();
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                case IS_DISCOVERING:
                    reply.writeNoException();
                    reply.writeBoolean(mDiscovering);
                    return true;
                case CREATE_BOND: {
                    String address = data.readString();
                    boolean le = data.readBoolean();
                    enforceAdmin();
                    boolean ok = createBond(address, le);
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                case REMOVE_BOND: {
                    String address = data.readString();
                    boolean ok = mayUnpair(address) && removeBond(address);
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                case FETCH_UUIDS: {
                    String address = data.readString();
                    enforceConnect();
                    boolean ok = fetchUuids(address);
                    reply.writeNoException();
                    reply.writeBoolean(ok);
                    return true;
                }
                default:
                    return false;
            }
        } catch (SecurityException | IllegalArgumentException e) {
            reply.writeException(e);
            return true;
        }
    }

    private static final String CONNECT = "android.permission.BLUETOOTH_CONNECT";
    private static final String SCAN = "android.permission.BLUETOOTH_SCAN";
    private static final String LEGACY = "android.permission.BLUETOOTH";
    private static final String ADMIN = "android.permission.BLUETOOTH_ADMIN";

    /** The calling package, or null for the system (and without a policy). */
    private String caller() {
        return mPolicy == null ? null : mPolicy.callerPackage();
    }

    private void require(String pkg, String modern, String legacy) {
        String permission = mPolicy.targetSdk(pkg) >= 31 ? modern : legacy;
        if (!mPolicy.granted(pkg, permission)) {
            throw new SecurityException("Need " + permission + " permission for " + pkg);
        }
    }

    /** As the module's startDiscovery/cancelDiscovery: BLUETOOTH_SCAN, or BLUETOOTH_ADMIN below 31. */
    private void enforceScan() {
        String pkg = caller();
        if (pkg != null) require(pkg, SCAN, ADMIN);
    }

    /** As the module's fetchUuidsWithSdp: BLUETOOTH_CONNECT, or BLUETOOTH below 31. */
    private void enforceConnect() {
        String pkg = caller();
        if (pkg != null) require(pkg, CONNECT, LEGACY);
    }

    /**
     * As the module's enable()/disable(): apps targeting API 33+ may not switch the radio
     * (false); below, BLUETOOTH_CONNECT (31+) or BLUETOOTH_ADMIN, and since this is the
     * PC's own radio, the user confirms (Gesher's consent dialog).
     */
    boolean mayToggle(boolean on) {
        String pkg = caller();
        if (pkg == null) return true;
        if (mPolicy.targetSdk(pkg) >= 33) {
            SystemServer.log("bluetooth: " + pkg + " (API 33+) may not turn the radio "
                    + (on ? "on" : "off"));
            return false;
        }
        require(pkg, CONNECT, ADMIN);
        WindowsBluetooth.Adapter adapter = WindowsBluetooth.adapter(0);
        if (!adapter.present || adapter.on == on) return true;  // nothing to switch
        boolean allowed = mPolicy.confirm(pkg, on ? "Turn Windows Bluetooth on" : "Turn Windows Bluetooth off");
        if (!allowed) SystemServer.log("bluetooth: the user did not let " + pkg + " switch the radio");
        return allowed;
    }

    /** As the module's createBond: BLUETOOTH_CONNECT, or BLUETOOTH_ADMIN below 31. */
    private void enforceAdmin() {
        String pkg = caller();
        if (pkg != null) require(pkg, CONNECT, ADMIN);
    }

    /**
     * removeBond is a system API in the module (BLUETOOTH_PRIVILEGED); apps reach it through
     * reflection. Here it removes Windows' pairing, so it needs BLUETOOTH_CONNECT (or
     * BLUETOOTH_ADMIN) and the user's consent.
     */
    boolean mayUnpair(String address) {
        String pkg = caller();
        if (pkg == null) return true;
        require(pkg, CONNECT, ADMIN);
        return mPolicy.confirm(pkg, "Unpair Bluetooth device " + address + " from Windows");
    }

    // --- bonds ---------------------------------------------------------------------------

    private WindowsBluetooth.Device bonded(long address) {
        synchronized (mLock) {
            return mDevices.get(address);
        }
    }

    /**
     * Starts pairing through Windows' own UI: ACTION_BOND_STATE_CHANGED BONDING now, then
     * BONDED (from the watch, which sees the new pairing) or back to NONE. False while the
     * radio is off, for a bonded device, or with a pairing of it already running.
     */
    public boolean createBond(String address, boolean le) {
        long value = WindowsBluetooth.address(address);
        if (value < 0) throw new IllegalArgumentException(address + " is not a Bluetooth address");
        if (state() != BluetoothAdapter.STATE_ON) return false;
        poll();
        if (bonded(value) != null) return false;
        synchronized (mLock) {
            if (!mPairing.add(value)) return false;
        }
        BluetoothDevice device = BluetoothDevice.createFromHost(address, BluetoothDevice.ADDRESS_TYPE_PUBLIC);
        bondIntent(device, BluetoothDevice.BOND_BONDING, BluetoothDevice.BOND_NONE);
        Thread t = new Thread(() -> {
            int result = 2;
            try {
                result = WindowsBluetooth.pair(value, le);
                SystemServer.log("bluetooth: pairing " + address + ": "
                        + (result == 0 ? "paired" : result == 1 ? "already paired" : "not paired"));
                poll();  // BONDED, when Windows now lists it
            } finally {
                boolean announced = bonded(value) != null;
                synchronized (mLock) {
                    mPairing.remove(value);
                }
                if (!announced) {
                    bondIntent(device, BluetoothDevice.BOND_NONE, BluetoothDevice.BOND_BONDING);
                }
            }
        }, "gesher-bluetooth-pair");
        t.setDaemon(true);
        t.start();
        return true;
    }

    /** Removes Windows' pairing; the watch broadcasts BOND_NONE. */
    public boolean removeBond(String address) {
        long value = WindowsBluetooth.address(address);
        if (value < 0) throw new IllegalArgumentException(address + " is not a Bluetooth address");
        if (state() != BluetoothAdapter.STATE_ON) return false;
        int result = WindowsBluetooth.unpair(value);
        poll();
        return result == 0;
    }

    private void bondIntent(BluetoothDevice device, int state, int previous) {
        Intent i = new Intent(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        i.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        i.putExtra(BluetoothDevice.EXTRA_BOND_STATE, state);
        i.putExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, previous);
        send(i);
    }

    // --- state ---------------------------------------------------------------------------

    /** BluetoothAdapter.STATE_* of Windows' radio now (no adapter: STATE_OFF). */
    public static int stateOf(WindowsBluetooth.Adapter adapter) {
        return adapter.present && adapter.on ? BluetoothAdapter.STATE_ON : BluetoothAdapter.STATE_OFF;
    }

    public int state() {
        synchronized (mLock) {
            if (mState != -1) return mState;
        }
        return stateOf(WindowsBluetooth.adapter(0));
    }

    public boolean setEnabled(boolean on) {
        WindowsBluetooth.Adapter adapter = WindowsBluetooth.adapter(0);
        if (!adapter.present) return false;
        if (adapter.on == on) return true;
        int turning = on ? BluetoothAdapter.STATE_TURNING_ON : BluetoothAdapter.STATE_TURNING_OFF;
        synchronized (mPollLock) {
            // The baseline first, so that the turn's devices are compared with what was.
            if (mState == -1) pollLocked();
            broadcastState(turning, stateOf(adapter));
            synchronized (mLock) {
                mState = turning;
            }
            int hr = WindowsBluetooth.setEnabled(on);
            SystemServer.log("bluetooth: turning the radio " + (on ? "on" : "off") + ": "
                    + WindowsBluetooth.hresult(hr));
            // ON/OFF (or, when Windows refused: Settings, airplane mode, policy, back to what
            // it is) comes from looking at the radio again.
            pollLocked();
            if (hr < 0) return false;
        }
        return true;
    }

    private void startWatch() {
        synchronized (mLock) {
            if (mWatch != null) return;
            mWatch = new Thread(() -> {
                for (;;) {
                    try {
                        poll();
                        // Windows' events end the wait early; a pairing made elsewhere
                        // (Settings) is seen within mPollMs.
                        WindowsBluetooth.waitForChange((int) mPollMs);
                    } catch (InterruptedException e) {
                        return;
                    } catch (RuntimeException e) {
                        SystemServer.log("bluetooth: watch: " + e);
                    }
                }
            }, "gesher-bluetooth-watch");
            mWatch.setDaemon(true);
            mWatch.start();
        }
        // The watch initializes its own baseline. Enumerating paired devices may
        // block in Windows; never perform that work on the app's Binder caller.
        // state() can answer from the adapter until the first poll finishes.
    }

    /** Looks at Windows once and broadcasts what changed. */
    public void poll() {
        synchronized (mPollLock) {
            pollLocked();
        }
    }

    private void pollLocked() {
        WindowsBluetooth.Adapter adapter = WindowsBluetooth.adapter(0);
        int state = stateOf(adapter);
        int before;
        synchronized (mLock) {
            before = mState;
            mState = state;
        }
        if (before == -1) {
            SystemServer.log("bluetooth: " + adapter.describe());
        } else if (before != state) {
            SystemServer.log("bluetooth: the radio is " + (state == BluetoothAdapter.STATE_ON ? "on" : "off"));
            if (state == BluetoothAdapter.STATE_ON) {
                if (before != BluetoothAdapter.STATE_TURNING_ON) {
                    broadcastState(BluetoothAdapter.STATE_TURNING_ON, before);
                }
                broadcastState(BluetoothAdapter.STATE_ON, BluetoothAdapter.STATE_TURNING_ON);
            } else {
                if (before != BluetoothAdapter.STATE_TURNING_OFF) {
                    broadcastState(BluetoothAdapter.STATE_TURNING_OFF, before);
                }
                broadcastState(BluetoothAdapter.STATE_OFF, BluetoothAdapter.STATE_TURNING_OFF);
            }
        }
        Map<Long, WindowsBluetooth.Device> now = new HashMap<>();
        if (state == BluetoothAdapter.STATE_ON) {
            for (WindowsBluetooth.Device d : WindowsBluetooth.bonded()) now.put(d.address, d);
        }
        Map<Long, WindowsBluetooth.Device> old;
        synchronized (mLock) {
            old = mDevices;
            mDevices = now;
        }
        if (before == -1) return;  // the first look sets the baseline
        // Bonds are compared only while the radio stays on: Android keeps them across an
        // off/on cycle (Windows lists no devices while its radio is off).
        boolean bonds = before == BluetoothAdapter.STATE_ON && state == BluetoothAdapter.STATE_ON;
        for (WindowsBluetooth.Device d : now.values()) {
            WindowsBluetooth.Device was = old.get(d.address);
            if (was == null) {
                if (bonds) {
                    boolean pairing;
                    synchronized (mLock) {
                        pairing = mPairing.contains(d.address);
                    }
                    bond(d, BluetoothDevice.BOND_BONDED,
                            pairing ? BluetoothDevice.BOND_BONDING : BluetoothDevice.BOND_NONE);
                }
                if (d.connected) connection(d, true);
            } else if (was.connected != d.connected) {
                connection(d, d.connected);
            }
        }
        for (WindowsBluetooth.Device was : old.values()) {
            if (now.containsKey(was.address)) continue;
            if (was.connected) connection(was, false);
            if (bonds) bond(was, BluetoothDevice.BOND_NONE, BluetoothDevice.BOND_BONDED);
        }
    }

    /** The paired devices the watch saw last. */
    public List<WindowsBluetooth.Device> devices() {
        synchronized (mLock) {
            return new ArrayList<>(mDevices.values());
        }
    }

    // --- broadcasts ----------------------------------------------------------------------

    private void send(Intent intent) {
        try {
            mBroadcaster.send(intent);
        } catch (RuntimeException e) {
            SystemServer.log("bluetooth: " + intent.getAction() + " failed: " + e);
        }
    }

    private void broadcastState(int state, int previous) {
        Intent i = new Intent(BluetoothAdapter.ACTION_STATE_CHANGED);
        i.putExtra(BluetoothAdapter.EXTRA_STATE, state);
        i.putExtra(BluetoothAdapter.EXTRA_PREVIOUS_STATE, previous);
        send(i);
    }

    static BluetoothDevice device(WindowsBluetooth.Device d) {
        return BluetoothDevice.createFromHost(WindowsBluetooth.address(d.address), d.addressType);
    }

    private void bond(WindowsBluetooth.Device d, int state, int previous) {
        Intent i = new Intent(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        i.putExtra(BluetoothDevice.EXTRA_DEVICE, device(d));
        i.putExtra(BluetoothDevice.EXTRA_BOND_STATE, state);
        i.putExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, previous);
        send(i);
    }

    private void connection(WindowsBluetooth.Device d, boolean connected) {
        BluetoothDevice device = device(d);
        Intent acl = new Intent(connected ? BluetoothDevice.ACTION_ACL_CONNECTED
                : BluetoothDevice.ACTION_ACL_DISCONNECTED);
        acl.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        acl.putExtra(BluetoothDevice.EXTRA_TRANSPORT,
                (d.kind & WindowsBluetooth.KIND_CLASSIC) != 0 ? BluetoothDevice.TRANSPORT_BREDR
                        : BluetoothDevice.TRANSPORT_LE);
        send(acl);
        int state = connected ? BluetoothProfile.STATE_CONNECTED : BluetoothProfile.STATE_DISCONNECTED;
        int previous = connected ? BluetoothProfile.STATE_DISCONNECTED : BluetoothProfile.STATE_CONNECTED;
        if (isA2dp(d)) profile(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED, device, state, previous);
        if (isHeadset(d)) profile(BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED, device, state, previous);
        Intent adapter = new Intent(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED);
        adapter.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        adapter.putExtra(BluetoothAdapter.EXTRA_CONNECTION_STATE, state);
        adapter.putExtra(BluetoothAdapter.EXTRA_PREVIOUS_CONNECTION_STATE, previous);
        send(adapter);
    }

    private void profile(String action, BluetoothDevice device, int state, int previous) {
        Intent i = new Intent(action);
        i.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        i.putExtra(BluetoothProfile.EXTRA_STATE, state);
        i.putExtra(BluetoothProfile.EXTRA_PREVIOUS_STATE, previous);
        send(i);
    }

    /** A device Windows plays audio to (A2DP sink). */
    public static boolean isA2dp(WindowsBluetooth.Device d) {
        return d.has(AUDIO_SINK) || d.has(ADVANCED_AUDIO);
    }

    /** A device with a hands-free or headset service. */
    public static boolean isHeadset(WindowsBluetooth.Device d) {
        return d.has(HANDSFREE) || d.has(HEADSET);
    }

    // --- discovery ---------------------------------------------------------------------------

    public boolean startDiscovery() {
        if (state() != BluetoothAdapter.STATE_ON) return false;
        synchronized (mLock) {
            if (mDiscovery != null) return true;
            mDiscovering = true;
            mDiscovery = new Thread(this::discover, "gesher-bluetooth-discovery");
            mDiscovery.setDaemon(true);
        }
        send(new Intent(BluetoothAdapter.ACTION_DISCOVERY_STARTED));
        mDiscovery.start();
        return true;
    }

    private void discover() {
        try {
            List<WindowsBluetooth.Device> found = WindowsBluetooth.inquiry(DISCOVERY_SECONDS);
            SystemServer.log("bluetooth: discovery found " + found.size() + " device(s)");
            for (WindowsBluetooth.Device d : found) {
                if (!mDiscovering) break;
                Intent i = new Intent(BluetoothDevice.ACTION_FOUND);
                i.putExtra(BluetoothDevice.EXTRA_DEVICE, device(d));
                i.putExtra(BluetoothDevice.EXTRA_CLASS, new BluetoothClass(d.classOfDevice));
                i.putExtra(BluetoothDevice.EXTRA_RSSI, Short.MIN_VALUE);
                if (!d.name.isEmpty()) i.putExtra(BluetoothDevice.EXTRA_NAME, d.name);
                send(i);
            }
        } finally {
            synchronized (mLock) {
                mDiscovering = false;
                mDiscovery = null;
            }
            send(new Intent(BluetoothAdapter.ACTION_DISCOVERY_FINISHED));
        }
    }

    public boolean cancelDiscovery() {
        synchronized (mLock) {
            if (mDiscovery == null) return true;
            mDiscovering = false;
        }
        WindowsBluetooth.cancelInquiry();
        return true;
    }

    public boolean isDiscovering() {
        return mDiscovering;
    }

    // --- SDP lookups ---------------------------------------------------------------------

    private static final int SDP_THREADS = 2;
    /** Distinct devices waiting for a free lookup thread; further ones are refused. */
    private static final int SDP_QUEUE = 64;
    private static final long SDP_KEEP_ALIVE_SECONDS = 15;
    private final Object mSdpLock = new Object();
    /** Devices with a lookup queued or running, and how many fetchUuids calls it answers. */
    private final Map<Long, Integer> mSdpInFlight = new HashMap<>();
    private final ThreadPoolExecutor mSdpExecutor = newSdpExecutor();

    private static ThreadPoolExecutor newSdpExecutor() {
        AtomicInteger count = new AtomicInteger();
        ThreadPoolExecutor executor = new ThreadPoolExecutor(SDP_THREADS, SDP_THREADS,
                SDP_KEEP_ALIVE_SECONDS, TimeUnit.SECONDS, new LinkedBlockingQueue<>(SDP_QUEUE),
                task -> {
                    Thread t = new Thread(task, "gesher-bluetooth-sdp-" + count.incrementAndGet());
                    t.setDaemon(true);
                    return t;
                });
        executor.allowCoreThreadTimeOut(true);
        return executor;
    }

    /**
     * Asks the device for its SDP records again, then broadcasts ACTION_UUID. Calls for a
     * device whose lookup is already queued or running share that one lookup, and each of
     * them still gets its broadcast. False while the radio is off, or when too many other
     * devices are waiting for a lookup.
     */
    public boolean fetchUuids(String address) {
        long value = WindowsBluetooth.address(address);
        if (value < 0) throw new IllegalArgumentException(address + " is not a Bluetooth address");
        if (state() != BluetoothAdapter.STATE_ON) return false;
        synchronized (mSdpLock) {
            Integer waiting = mSdpInFlight.get(value);
            if (waiting != null) {
                mSdpInFlight.put(value, waiting + 1);
                return true;
            }
            // Mark the device before submitting: the lookup may finish (and read this
            // entry) before execute() returns.
            mSdpInFlight.put(value, 1);
            try {
                mSdpExecutor.execute(() -> lookUpUuids(address, value));
            } catch (RejectedExecutionException e) {
                mSdpInFlight.remove(value);
                return false;
            }
        }
        return true;
    }

    /** Ends the device's lookup; the number of calls it answered. */
    private int finishSdp(long value) {
        synchronized (mSdpLock) {
            Integer waiting = mSdpInFlight.remove(value);
            return waiting == null ? 0 : waiting;
        }
    }

    private void lookUpUuids(String address, long value) {
        boolean finished = false;
        try {
            List<UUID> uuids = WindowsBluetooth.remoteUuids(value, true);
            // From here a new fetchUuids starts its own lookup.
            int callers = finishSdp(value);
            finished = true;
            for (int n = 0; n < callers; n++) {
                Intent i = new Intent(BluetoothDevice.ACTION_UUID);
                i.putExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.createFromHost(address,
                        BluetoothDevice.ADDRESS_TYPE_PUBLIC));
                Parcelable[] list = new Parcelable[uuids.size()];
                for (int k = 0; k < list.length; k++) list[k] = new ParcelUuid(uuids.get(k));
                i.putExtra(BluetoothDevice.EXTRA_UUID, list);
                send(i);
            }
        } catch (RuntimeException e) {
            SystemServer.log("bluetooth: SDP lookup of " + address + " failed: " + e);
        } finally {
            // A failed lookup must not leave the device marked busy.
            if (!finished) finishSdp(value);
        }
    }

    // --- the app side ------------------------------------------------------------------------

    /** Calls the system server's service; answers "none" where there is no system server. */
    public static final class Client {
        private Client() {
        }

        private static IBinder service() {
            try {
                return ServiceManager.getService(NAME);
            } catch (RuntimeException e) {
                return null;
            }
        }

        public static boolean available() {
            return service() != null;
        }

        /** {state, discovering}; without the service, the radio's state from Windows. */
        public static int[] state() {
            IBinder b = service();
            if (b == null) return new int[] {stateOf(WindowsBluetooth.adapter()), 0};
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                b.transact(STATE, data, reply, 0);
                reply.readException();
                int state = reply.readInt();
                return new int[] {state, reply.readBoolean() ? 1 : 0};
            } catch (RemoteException e) {
                return new int[] {stateOf(WindowsBluetooth.adapter()), 0};
            } finally {
                data.recycle();
                reply.recycle();
            }
        }

        private static boolean call(int code, Object... arguments) {
            IBinder b = service();
            if (b == null) return false;
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                for (Object argument : arguments) {
                    if (argument instanceof Boolean) data.writeBoolean((Boolean) argument);
                    if (argument instanceof String) data.writeString((String) argument);
                }
                b.transact(code, data, reply, 0);
                reply.readException();
                return reply.readBoolean();
            } catch (RemoteException e) {
                return false;
            } finally {
                data.recycle();
                reply.recycle();
            }
        }

        public static boolean setEnabled(boolean on) {
            if (service() == null) {
                // No system server (a bare runtime): straight to Windows.
                return WindowsBluetooth.setEnabled(on) >= 0;
            }
            return call(SET_ENABLED, on);
        }

        public static boolean startDiscovery() {
            return call(START_DISCOVERY);
        }

        public static boolean cancelDiscovery() {
            return call(CANCEL_DISCOVERY);
        }

        public static boolean isDiscovering() {
            return call(IS_DISCOVERING);
        }

        public static boolean createBond(String address, boolean le) {
            return call(CREATE_BOND, address, le);
        }

        public static boolean removeBond(String address) {
            return call(REMOVE_BOND, address);
        }

        public static boolean fetchUuids(String address) {
            return call(FETCH_UUIDS, address);
        }
    }
}
