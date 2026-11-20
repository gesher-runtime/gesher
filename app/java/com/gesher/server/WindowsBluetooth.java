/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;

/**
 * The host's Bluetooth radio (compat/devices/bluetooth_jni.cc over bluetooth_win.cc:
 * Windows.Devices.Bluetooth, BluetoothApis.dll and Winsock's AF_BTH; or, with
 * {@code GESHER_BLUETOOTH_TEST=1}, bluetooth_test.cc's test radio), for the
 * android.bluetooth classes (app process) and {@link BluetoothManagerService} (system
 * server). See docs/bluetooth.md.
 *
 * <p>Natives are in dalvikvm.exe; a runtime without them (UnsatisfiedLinkError) has no
 * adapter. Addresses are 48-bit values; {@link #address(long)} spells them as Android does.
 */
public final class WindowsBluetooth {
    private WindowsBluetooth() {
    }

    // Device kinds (bluetooth.h).
    public static final int KIND_CLASSIC = 1;
    public static final int KIND_LE = 2;

    // GATT event types (bluetooth.h).
    public static final int EVENT_NONE = 0;
    public static final int EVENT_NOTIFICATION = 1;
    public static final int EVENT_CONNECTION = 2;
    public static final int EVENT_MTU = 3;
    public static final int EVENT_SERVICES_CHANGED = 4;
    public static final int EVENT_CLOSED = 5;

    // GATT attribute kinds (bluetooth.h).
    public static final int ATTRIBUTE_SERVICE = 0;
    public static final int ATTRIBUTE_CHARACTERISTIC = 1;
    public static final int ATTRIBUTE_DESCRIPTOR = 2;
    public static final int ATTRIBUTE_INCLUDED = 3;

    /** The local radio. */
    public static final class Adapter {
        public final boolean present;
        public final boolean on;
        public final long address;
        public final String name;
        public final int flags;
        public final int maxAdvertisingDataLength;
        public final String via;
        public final int hr;

        Adapter(boolean present, boolean on, long address, String name, int flags, int maxAdv,
                String via, int hr) {
            this.present = present;
            this.on = on;
            this.address = address;
            this.name = name;
            this.flags = flags;
            this.maxAdvertisingDataLength = maxAdv;
            this.via = via;
            this.hr = hr;
        }

        public boolean classic() { return (flags & 1) != 0; }
        public boolean le() { return (flags & 2) != 0; }
        public boolean central() { return (flags & 4) != 0; }
        public boolean peripheral() { return (flags & 8) != 0; }
        public boolean extendedAdvertising() { return (flags & 16) != 0; }
        public boolean le2mPhy() { return (flags & 32) != 0; }
        public boolean leCodedPhy() { return (flags & 64) != 0; }

        public String describe() {
            if (!present) return "no adapter (" + hresult(hr) + ", via " + via + ")";
            return address(address) + " \"" + name + "\" " + (on ? "on" : "off") + " via " + via;
        }
    }

    /** A paired or discovered remote device. */
    public static final class Device {
        public final long address;
        public final int addressType;
        public final int kind;
        public final boolean bonded;
        public final boolean connected;
        public final int classOfDevice;
        public final String name;
        public final List<UUID> uuids;

        Device(long address, int addressType, int kind, boolean bonded, boolean connected,
                int classOfDevice, String name, List<UUID> uuids) {
            this.address = address;
            this.addressType = addressType;
            this.kind = kind;
            this.bonded = bonded;
            this.connected = connected;
            this.classOfDevice = classOfDevice;
            this.name = name;
            this.uuids = uuids;
        }

        public boolean has(UUID uuid) {
            return uuids.contains(uuid);
        }

        /** Connection and bond state, for change detection. */
        String state() {
            return address + ":" + bonded + ":" + connected + ":" + name + ":" + uuids;
        }
    }

    private static volatile Adapter sAdapter;
    private static volatile long sAdapterAt;

    /** Recent observation for host diagnostics; never activates Windows or blocks. */
    public static Adapter cachedAdapter(long maxAgeMs) {
        Adapter a = sAdapter;
        long age = System.nanoTime() / 1_000_000 - sAdapterAt;
        return a != null && age >= 0 && age <= maxAgeMs ? a : null;
    }

    /** The adapter, asked again after {@code maxAgeMs}. */
    public static Adapter adapter(long maxAgeMs) {
        Adapter a = sAdapter;
        // System.nanoTime, not SystemClock: host-JVM tests load the system server's classes.
        long now = System.nanoTime() / 1_000_000;
        if (a != null && now - sAdapterAt <= maxAgeMs) return a;
        a = readAdapter();
        sAdapter = a;
        sAdapterAt = now;
        return a;
    }

    /** The adapter as Windows (or the test radio) answers now. */
    public static Adapter adapter() {
        return adapter(1000);
    }

    private static Adapter readAdapter() {
        String line;
        try {
            line = nativeAdapter();
        } catch (UnsatisfiedLinkError e) {
            return new Adapter(false, false, 0, "", 0, 31, "no natives", 0x80004001);
        }
        String[] f = line.split("\t", -1);
        if (f.length < 8) return new Adapter(false, false, 0, "", 0, 31, "bad answer", 0x80004005);
        return new Adapter("1".equals(f[0]), "1".equals(f[1]), Long.parseLong(f[2]), f[3],
                Integer.parseInt(f[4]), Integer.parseInt(f[5]), f[6], (int) Long.parseLong(f[7]));
    }

    /** Forgets the cached adapter (after turning the radio on or off). */
    public static void invalidate() {
        sAdapter = null;
    }

    public static int setEnabled(boolean on) {
        try {
            return nativeSetEnabled(on);
        } catch (UnsatisfiedLinkError e) {
            return 0x80004001;
        } finally {
            invalidate();
        }
    }

    public static List<Device> bonded() {
        try {
            return devices(nativeBonded(new int[1]));
        } catch (UnsatisfiedLinkError e) {
            return new ArrayList<>();
        }
    }

    /** A classic inquiry of about {@code seconds} (blocks); {@link #cancelInquiry} ends it early. */
    public static List<Device> inquiry(int seconds) {
        try {
            return devices(nativeInquiry(seconds, new int[1]));
        } catch (UnsatisfiedLinkError e) {
            return new ArrayList<>();
        }
    }

    public static void cancelInquiry() {
        try {
            nativeInquiryCancel();
        } catch (UnsatisfiedLinkError ignored) {
            // no inquiry
        }
    }

    /** A device's SDP service classes; {@code refresh} asks the device again (slow). */
    public static List<UUID> remoteUuids(long address, boolean refresh) {
        List<UUID> out = new ArrayList<>();
        try {
            for (String u : nativeRemoteUuids(address, refresh, new int[1])) out.add(UUID.fromString(u));
        } catch (UnsatisfiedLinkError | IllegalArgumentException e) {
            // none known
        }
        return out;
    }

    private static List<Device> devices(String[] lines) {
        List<Device> out = new ArrayList<>();
        for (String line : lines) {
            String[] f = line.split("\t", -1);
            if (f.length < 8) continue;
            List<UUID> uuids = new ArrayList<>();
            for (String u : f[7].split(",")) {
                if (u.isEmpty()) continue;
                try {
                    uuids.add(UUID.fromString(u));
                } catch (IllegalArgumentException ignored) {
                    // skipped
                }
            }
            out.add(new Device(Long.parseLong(f[0]), Integer.parseInt(f[1]), Integer.parseInt(f[2]),
                    "1".equals(f[3]), "1".equals(f[4]), (int) Long.parseLong(f[5]), f[6], uuids));
        }
        return out;
    }

    /** Pairs through Windows' own UI (blocks): 0 paired, 1 already paired, 2 not paired. */
    public static int pair(long address, boolean le) {
        int[] result = new int[] {2};
        try {
            nativePair(address, le, result);
        } catch (UnsatisfiedLinkError e) {
            return 2;
        }
        return result[0];
    }

    /** Removes Windows' pairing: 0 unpaired, 1 was not paired, 2 failed. */
    public static int unpair(long address) {
        int[] result = new int[] {2};
        try {
            nativeUnpair(address, result);
        } catch (UnsatisfiedLinkError e) {
            return 2;
        }
        return result[0];
    }

    /**
     * Waits up to {@code timeoutMs} for a change Windows announces (radio state, a paired
     * device's connection, a pairing); true when one came. Without natives it just waits.
     */
    public static boolean waitForChange(int timeoutMs) throws InterruptedException {
        try {
            return nativeWaitForChange(timeoutMs);
        } catch (UnsatisfiedLinkError e) {
            Thread.sleep(timeoutMs);
            return false;
        }
    }

    // --- addresses ------------------------------------------------------------------------

    /** "AA:BB:CC:DD:EE:FF" (upper case, as BluetoothAdapter.checkBluetoothAddress wants). */
    public static String address(long address) {
        StringBuilder s = new StringBuilder(17);
        for (int shift = 40; shift >= 0; shift -= 8) {
            if (s.length() > 0) s.append(':');
            s.append(String.format(Locale.ROOT, "%02X", (address >>> shift) & 0xff));
        }
        return s.toString();
    }

    /** The 48-bit value of "AA:BB:CC:DD:EE:FF"; -1 when it is not one. */
    public static long address(String text) {
        if (text == null || text.length() != 17) return -1;
        long value = 0;
        for (int i = 0; i < 6; i++) {
            try {
                value = value << 8 | Integer.parseInt(text.substring(i * 3, i * 3 + 2), 16);
            } catch (NumberFormatException e) {
                return -1;
            }
            if (i < 5 && text.charAt(i * 3 + 2) != ':') return -1;
        }
        return value;
    }

    public static String hresult(int hr) {
        return String.format(Locale.ROOT, "HRESULT 0x%08X", hr);
    }

    // --- natives -----------------------------------------------------------------------------

    static native String nativeAdapter();
    static native int nativeSetEnabled(boolean on);
    static native String[] nativeBonded(int[] hr);
    static native String[] nativeInquiry(int seconds, int[] hr);
    static native void nativeInquiryCancel();
    static native String[] nativeRemoteUuids(long address, boolean refresh, int[] hr);
    static native int nativePair(long address, boolean le, int[] result);
    static native int nativeUnpair(long address, int[] result);
    static native boolean nativeWaitForChange(int timeoutMs);

    public static native long nativeScanStart(boolean active, int[] hr);
    public static native byte[] nativeScanNext(long scan, int timeoutMs, long[] meta);
    public static native void nativeScanStop(long scan);

    public static native long nativeGattOpen(long address, int addressType, int[] hr);
    public static native int nativeGattConnect(long gatt, int timeoutMs, int[] mtu);
    public static native String[] nativeGattDiscover(long gatt, int[] status);
    public static native byte[] nativeGattRead(long gatt, int kind, int handle, int[] status);
    public static native int nativeGattWrite(long gatt, int kind, int handle, byte[] value,
            boolean withResponse);
    public static native int nativeGattNotify(long gatt, int handle, int mode);
    public static native int nativeGattMtu(long gatt);
    public static native byte[] nativeGattNext(long gatt, int timeoutMs, int[] meta);
    public static native int nativeGattPhy(long gatt, int[] phy);
    public static native void nativeGattClose(long gatt);

    public static native long nativeRfcommOpen(long address, String uuid, int channel,
            boolean secure, int[] hr);
    public static native int nativeRfcommConnect(long socket, int[] channel);
    public static native int nativeRfcommRead(long socket, byte[] buffer, int offset, int length);
    public static native int nativeRfcommWrite(long socket, byte[] buffer, int offset, int length);
    public static native int nativeRfcommAvailable(long socket);
    public static native void nativeRfcommClose(long socket);
    public static native long nativeRfcommListen(String name, String uuid, boolean secure,
            int[] out);
    public static native long nativeRfcommAccept(long server, int timeoutMs, long[] out);
    public static native void nativeRfcommServerClose(long server);
}
