/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.PendingIntent;
import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothHost;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.ParcelUuid;
import android.os.Parcelable;
import android.os.ServiceManager;
import com.gesher.server.BluetoothManagerService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.WindowsBluetooth;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * The android.bluetooth classes end to end over GESHER_BLUETOOTH_TEST's radio
 * (compat/devices/bluetooth_test.cc, below the JNI layer; docs/bluetooth.md), in one bare
 * process with BluetoothManagerService installed in-process: permissions, adapter state,
 * paired devices, profiles, LE scan, GATT, RFCOMM client and listener, discovery and the
 * system's broadcasts. With {@code --no-radio} (no test radio) it checks that a PC without
 * a radio has no adapter and prints what Windows answered.
 */
public final class BluetoothHostTest {
    private static int checks;
    private static final UUID SPP = UUID.fromString("00001101-0000-1000-8000-00805f9b34fb");
    private static final UUID VENDOR = UUID.fromString("0cf12d31-fac3-4553-bd80-d6832e70f1a5");
    private static final UUID BATTERY = UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb");
    private static final UUID LEVEL = UUID.fromString("00002a19-0000-1000-8000-00805f9b34fb");
    private static final UUID UART = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final String HEADSET = "00:02:5B:00:A5:A5";
    private static final String PERIPHERAL = "C0:FF:EE:00:00:01";

    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        checks++;
    }

    private static void step(String text) {
        System.out.println("BluetoothHostTest: " + text);
    }

    /** An app context with the given manifest permissions, target SDK and package. */
    static final class App extends ContextWrapper {
        private final Set<String> mGranted;
        private final int mTarget;

        App(int target, String... granted) {
            super(null);
            mTarget = target;
            mGranted = new HashSet<>(Arrays.asList(granted));
        }

        @Override
        public int checkSelfPermission(String permission) {
            return mGranted.contains(permission) ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }

        @Override
        public String getPackageName() {
            return "fixture.bluetooth";
        }

        @Override
        public ApplicationInfo getApplicationInfo() {
            ApplicationInfo info = new ApplicationInfo();
            info.targetSdkVersion = mTarget;
            info.packageName = getPackageName();
            return info;
        }
    }

    private static <T> T take(LinkedBlockingQueue<T> queue, String what) throws InterruptedException {
        T value = queue.poll(10, TimeUnit.SECONDS);
        check(value != null, "timed out waiting for " + what);
        return value;
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        if (args.length > 0 && args[0].equals("--no-radio")) {
            noRadio();
            return;
        }
        LinkedBlockingQueue<Intent> broadcasts = new LinkedBlockingQueue<>();
        BluetoothManagerService service = new BluetoothManagerService(broadcasts::add, null);
        service.setPollInterval(200);
        ServiceManager.initServiceCache(Map.of(BluetoothManagerService.NAME, service));

        java.lang.reflect.Field watch = BluetoothManagerService.class.getDeclaredField("mWatch");
        watch.setAccessible(true);
        check(watch.get(service) == null, "radio watch is lazy before any adapter access");
        check(BluetoothAdapter.getDefaultAdapter() != null && watch.get(service) != null,
                "obtaining an adapter starts the watch for external radio/device broadcasts");

        features(true);
        permissions();
        servicePolicy();
        App app = new App(34, "android.permission.BLUETOOTH_CONNECT",
                "android.permission.BLUETOOTH_SCAN", "android.permission.ACCESS_FINE_LOCATION");
        BluetoothAdapter adapter = new BluetoothManager(app).getAdapter();
        adapterAndDevices(adapter);
        profiles(adapter, app);
        scan(adapter);
        gatt(adapter, app);
        rfcomm(adapter);
        broadcastsAndDiscovery(adapter, service, broadcasts);
        System.out.println("BluetoothHostTest all passed: " + checks);
        System.exit(0);
    }

    private static void noRadio() {
        WindowsBluetooth.Adapter a = WindowsBluetooth.adapter(0);
        step("Windows: " + a.describe());
        check(!a.present, "a CI runner has no Bluetooth radio");
        check(BluetoothAdapter.getDefaultAdapter() == null, "no default adapter without a radio");
        check(new BluetoothManager(new App(34)).getAdapter() == null, "no manager adapter without a radio");
        features(false);
        System.out.println("BluetoothHostTest no radio passed: " + checks);
    }

    /** FEATURE_BLUETOOTH(_LE) follow the host radio (the test radio supports LE). */
    private static void features(boolean radio) {
        PackageManagerService pm = new PackageManagerService(List.of());
        boolean classic = pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH, 0);
        boolean le = pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE, 0);
        Set<String> listed = new HashSet<>();
        for (android.content.pm.FeatureInfo f : pm.getSystemAvailableFeatures().getList()) listed.add(f.name);
        check(classic == radio && le == radio, "hasSystemFeature(bluetooth, bluetooth_le) = " + radio);
        check(listed.contains(PackageManager.FEATURE_BLUETOOTH) == radio
                && listed.contains(PackageManager.FEATURE_BLUETOOTH_LE) == radio,
                "getSystemAvailableFeatures lists them exactly when there is a radio");
        step("features: bluetooth " + classic + ", bluetooth_le " + le);
    }

    /** A Binder caller of gesher.bluetooth with this package, target SDK and permissions. */
    static final class Caller implements BluetoothManagerService.Policy {
        final int target;
        final Set<String> granted;
        final List<String> asked = new ArrayList<>();
        boolean consent = true;

        Caller(int target, String... granted) {
            this.target = target;
            this.granted = new HashSet<>(Arrays.asList(granted));
        }

        @Override public String callerPackage() { return "fixture.caller"; }
        @Override public int targetSdk(String packageName) { return target; }
        @Override public boolean granted(String packageName, String permission) {
            return granted.contains(permission);
        }
        @Override public boolean confirm(String packageName, String what) {
            asked.add(what);
            return consent;
        }
    }

    private static boolean transact(BluetoothManagerService service, int code, Object argument)
            throws Exception {
        android.os.Parcel data = android.os.Parcel.obtain();
        android.os.Parcel reply = android.os.Parcel.obtain();
        try {
            data.writeInterfaceToken(BluetoothManagerService.DESCRIPTOR);
            if (argument instanceof Boolean) data.writeBoolean((Boolean) argument);
            if (argument instanceof String) data.writeString((String) argument);
            service.transact(code, data, reply, 0);
            reply.readException();
            return reply.readBoolean();
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    private static void rejected(BluetoothManagerService service, int code, Object argument,
            String permission, String what) throws Exception {
        try {
            transact(service, code, argument);
            check(false, what + " without " + permission);
        } catch (SecurityException expected) {
            check(expected.getMessage().contains(permission), what + " needs " + permission + ": " + expected);
        }
    }

    /** gesher.bluetooth checks Binder callers as the module's service does. */
    private static void servicePolicy() throws Exception {
        final int setEnabled = android.os.IBinder.FIRST_CALL_TRANSACTION + 1;
        final int startDiscovery = android.os.IBinder.FIRST_CALL_TRANSACTION + 2;
        final int fetchUuids = android.os.IBinder.FIRST_CALL_TRANSACTION + 5;
        List<Intent> sent = new ArrayList<>();
        // The normal BLUETOOTH permission does not switch the radio.
        BluetoothManagerService legacy = new BluetoothManagerService(sent::add,
                new Caller(30, "android.permission.BLUETOOTH"));
        rejected(legacy, setEnabled, false, "android.permission.BLUETOOTH_ADMIN", "radio off (API 30)");
        rejected(legacy, startDiscovery, null, "android.permission.BLUETOOTH_ADMIN", "discovery (API 30)");
        final int createBond = android.os.IBinder.FIRST_CALL_TRANSACTION + 6;
        final int removeBond = android.os.IBinder.FIRST_CALL_TRANSACTION + 7;
        rejected(legacy, createBond, HEADSET, "android.permission.BLUETOOTH_ADMIN", "pairing (API 30)");
        Caller declines = new Caller(31, "android.permission.BLUETOOTH_CONNECT");
        declines.consent = false;
        check(!transact(new BluetoothManagerService(sent::add, declines), removeBond, HEADSET)
                && declines.asked.size() == 1 && declines.asked.get(0).startsWith("Unpair"),
                "unpairing asks the user; declined, nothing happens");
        // API 31+: BLUETOOTH_CONNECT and BLUETOOTH_SCAN.
        BluetoothManagerService modern = new BluetoothManagerService(sent::add,
                new Caller(32, "android.permission.BLUETOOTH_ADMIN", "android.permission.BLUETOOTH"));
        rejected(modern, setEnabled, false, "android.permission.BLUETOOTH_CONNECT", "radio off (API 32)");
        rejected(modern, startDiscovery, null, "android.permission.BLUETOOTH_SCAN", "discovery (API 32)");
        rejected(modern, fetchUuids, HEADSET, "android.permission.BLUETOOTH_CONNECT", "SDP (API 32)");
        // API 33+ apps may not switch the radio at all, whatever they hold.
        Caller tiramisu = new Caller(34, "android.permission.BLUETOOTH_CONNECT");
        check(!transact(new BluetoothManagerService(sent::add, tiramisu), setEnabled, false),
                "API 33+ callers cannot turn the radio off");
        check(tiramisu.asked.isEmpty(), "and are not even asked about");
        // Allowed callers still need the user's consent; a refusal leaves the radio on.
        Caller refused = new Caller(30, "android.permission.BLUETOOTH_ADMIN");
        refused.consent = false;
        check(!transact(new BluetoothManagerService(sent::add, refused), setEnabled, false)
                && refused.asked.equals(List.of("Turn Windows Bluetooth off"))
                && WindowsBluetooth.adapter(0).on, "the user declines: the radio stays on");
        Caller scanner = new Caller(31, "android.permission.BLUETOOTH_SCAN");
        BluetoothManagerService discovery = new BluetoothManagerService(sent::add, scanner);
        check(transact(discovery, startDiscovery, null), "BLUETOOTH_SCAN starts a discovery");
        check(transact(discovery, startDiscovery + 1, null), "and cancels it");
        step("service policy: BLUETOOTH cannot switch the radio, API 31+ needs CONNECT/SCAN, API 33+ refused, consent asked");
    }

    private static void permissions() throws Exception {
        App none = new App(34);
        BluetoothAdapter adapter = new BluetoothManager(none).getAdapter();
        check(adapter != null && adapter.isEnabled() && adapter.getState() == BluetoothAdapter.STATE_ON,
                "the test radio's adapter is on, without permissions as well");
        try {
            adapter.getName();
            check(false, "getName without BLUETOOTH_CONNECT");
        } catch (SecurityException expected) {
            check(expected.getMessage().contains("BLUETOOTH_CONNECT"), "the exception names BLUETOOTH_CONNECT");
        }
        try {
            adapter.getBluetoothLeScanner().startScan(new ScanCallback() {});
            check(false, "startScan without BLUETOOTH_SCAN");
        } catch (SecurityException expected) {
            check(expected.getMessage().contains("BLUETOOTH_SCAN"), "the exception names BLUETOOTH_SCAN");
        }
        // Android rejects unsupported settings locally before the registration
        // service enforces BLUETOOTH_SCAN, including for a caller without it.
        BluetoothLeScanner deniedScanner = adapter.getBluetoothLeScanner();
        LinkedBlockingQueue<Integer> failures = new LinkedBlockingQueue<>();
        ScanSettings unsupported = new ScanSettings.Builder()
                .setCallbackType(ScanSettings.CALLBACK_TYPE_FIRST_MATCH).build();
        deniedScanner.startScan(null, unsupported, new ScanCallback() {
            @Override public void onScanFailed(int error) { failures.add(error); }
        });
        check(take(failures, "unsupported settings without scan permission")
                == ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED,
                "unsupported settings take precedence over a missing scan permission");
        PendingIntent deniedIntent = new PendingIntent(new android.content.IIntentSender.Stub() {
            @Override public void send(int code, Intent intent, String type, android.os.IBinder allowlist,
                    android.content.IIntentReceiver finished, String permission, android.os.Bundle options) {
                throw new AssertionError("Rejected scan must not send its PendingIntent");
            }
        });
        check(deniedScanner.startScan(null, unsupported, deniedIntent)
                == ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED,
                "unsupported PendingIntent settings precede scan permission enforcement");
        try {
            deniedScanner.startScan(null, null, deniedIntent);
            check(false, "supported PendingIntent scan without BLUETOOTH_SCAN");
        } catch (SecurityException expected) {
            check(expected.getMessage().contains("BLUETOOTH_SCAN"),
                    "supported PendingIntent scan still needs BLUETOOTH_SCAN");
        }
        // Apps targeting API 30 use the legacy BLUETOOTH permission.
        BluetoothAdapter legacy = new BluetoothManager(new App(30, "android.permission.BLUETOOTH")).getAdapter();
        check("Gesher Test Radio".equals(legacy.getName()), "API 30 app with BLUETOOTH reads the name");
        step("permissions: BLUETOOTH_CONNECT/BLUETOOTH_SCAN on API 31+, BLUETOOTH below");
    }

    private static void adapterAndDevices(BluetoothAdapter adapter) throws Exception {
        check("Gesher Test Radio".equals(adapter.getName()), "adapter name");
        check(BluetoothAdapter.DEFAULT_MAC_ADDRESS.equals(adapter.getAddress()), "the local address is hidden");
        check(adapter.getScanMode() == BluetoothAdapter.SCAN_MODE_CONNECTABLE, "connectable");
        Set<BluetoothDevice> bonded = adapter.getBondedDevices();
        check(bonded.size() == 1, "one bonded device: " + bonded);
        BluetoothDevice headset = bonded.iterator().next();
        check(HEADSET.equals(headset.getAddress()), "the headset's address");
        check("Gesher Test Headset".equals(headset.getName()), "the headset's name");
        check(headset.getType() == BluetoothDevice.DEVICE_TYPE_CLASSIC, "classic device");
        check(headset.getBondState() == BluetoothDevice.BOND_BONDED, "bonded");
        BluetoothClass cls = headset.getBluetoothClass();
        check(cls != null && cls.getMajorDeviceClass() == BluetoothClass.Device.Major.AUDIO_VIDEO
                && cls.hasService(BluetoothClass.Service.AUDIO), "audio/video class of device");
        ParcelUuid[] uuids = headset.getUuids();
        check(uuids != null && Arrays.asList(uuids).contains(new ParcelUuid(SPP))
                && Arrays.asList(uuids).contains(new ParcelUuid(VENDOR)), "SPP and vendor service classes");
        check((Boolean) BluetoothDevice.class.getMethod("isConnected").invoke(headset),
                "isConnected through reflection, as apps call it");
        check(!(Boolean) BluetoothDevice.class.getMethod("isEncrypted").invoke(headset),
                "no encryption claimed without a secure socket");
        check(adapter.getRemoteDevice(HEADSET).equals(headset), "getRemoteDevice equals the bonded device");
        check(!adapter.isLeExtendedAdvertisingSupported(), "extended advertisements are not exposed yet");
        check(!adapter.isOffloadedScanBatchingSupported(),
                "app-process report delays do not claim controller scan batching");
        check(adapter.getBluetoothLeAdvertiser() == null && adapter.getLeMaximumAdvertisingDataLength() == 0,
                "unavailable advertising returns no advertiser and no extended data capacity");
        for (int i = 0; i < 1024; i++) {
            BluetoothHost.sawLe(WindowsBluetooth.address(0xc00000000000L + i), "rotating " + i);
        }
        java.lang.reflect.Field cache = BluetoothHost.class.getDeclaredField("sKnown");
        cache.setAccessible(true);
        Map<?, ?> known = (Map<?, ?>) cache.get(null);
        synchronized (known) {
            check(known.size() <= 512 && known.containsKey(HEADSET),
                    "rotating LE addresses are bounded without evicting the bonded headset");
        }
        try {
            adapter.getRemoteDevice("00:11:22:33:44:5g");
            check(false, "an invalid address is rejected");
        } catch (IllegalArgumentException expected) {
            checks++;
        }
        step("adapter: \"Gesher Test Radio\" on, bonded headset 00:02:5B:00:A5:A5 (classic, audio/video, SPP + vendor service)");
    }

    private static void profiles(BluetoothAdapter adapter, Context app) throws Exception {
        LinkedBlockingQueue<BluetoothProfile> proxies = new LinkedBlockingQueue<>();
        BluetoothProfile.ServiceListener listener = new BluetoothProfile.ServiceListener() {
            @Override
            public void onServiceConnected(int profile, BluetoothProfile proxy) {
                proxies.add(proxy);
            }

            @Override
            public void onServiceDisconnected(int profile) {}
        };
        check(adapter.getProfileProxy(app, listener, BluetoothProfile.A2DP), "A2DP proxy");
        BluetoothProfile a2dp = take(proxies, "the A2DP proxy");
        check(a2dp instanceof BluetoothA2dp && a2dp.getConnectedDevices().size() == 1
                && a2dp.getConnectedDevices().get(0).getAddress().equals(HEADSET), "A2DP: the headset");
        check(adapter.getProfileProxy(app, listener, BluetoothProfile.HEADSET), "headset proxy");
        BluetoothProfile hfp = take(proxies, "the headset proxy");
        check(hfp.getConnectionState(adapter.getRemoteDevice(HEADSET)) == BluetoothProfile.STATE_CONNECTED,
                "hands-free: connected");
        check(adapter.getProfileConnectionState(BluetoothProfile.A2DP) == BluetoothProfile.STATE_CONNECTED,
                "adapter's A2DP state");
        check(!adapter.getProfileProxy(app, listener, BluetoothProfile.HID_DEVICE), "no HID device profile");
        step("profiles: A2DP and headset proxies list the connected headset");
    }

    private static void scan(BluetoothAdapter adapter) throws Exception {
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        LinkedBlockingQueue<ScanResult> matched = new LinkedBlockingQueue<>();
        LinkedBlockingQueue<ScanResult> other = new LinkedBlockingQueue<>();
        LinkedBlockingQueue<Integer> failures = new LinkedBlockingQueue<>();
        LinkedBlockingQueue<Thread> failureThreads = new LinkedBlockingQueue<>();
        LinkedBlockingQueue<String> unexpectedReports = new LinkedBlockingQueue<>();
        ScanCallback battery = new ScanCallback() {
            @Override
            public void onScanResult(int type, ScanResult result) {
                if (type == ScanSettings.CALLBACK_TYPE_ALL_MATCHES) matched.add(result);
                else unexpectedReports.add("callback type " + type);
            }

            @Override
            public void onBatchScanResults(List<ScanResult> results) {
                unexpectedReports.add("batch");
            }

            @Override
            public void onScanFailed(int error) {
                failureThreads.add(Thread.currentThread());
                failures.add(error);
            }
        };
        ScanCallback heartRate = new ScanCallback() {
            @Override
            public void onScanResult(int type, ScanResult result) {
                other.add(result);
            }
        };
        ScanSettings fast = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        List<ScanFilter> batteryFilter =
                List.of(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(BATTERY)).build());
        check(!adapter.isOffloadedFilteringSupported(), "this backend does not expose controller-offloaded filtering");
        List<ScanSettings> unsupported = List.of(
                new ScanSettings.Builder().setCallbackType(ScanSettings.CALLBACK_TYPE_FIRST_MATCH).build(),
                new ScanSettings.Builder().setCallbackType(ScanSettings.CALLBACK_TYPE_MATCH_LOST).build(),
                new ScanSettings.Builder().setCallbackType(ScanSettings.CALLBACK_TYPE_FIRST_MATCH
                        | ScanSettings.CALLBACK_TYPE_MATCH_LOST).build(),
                new ScanSettings.Builder().setReportDelay(1).build(),
                new ScanSettings.Builder().setReportDelay(Long.MAX_VALUE).build(),
                new ScanSettings.Builder().setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES_AUTO_BATCH)
                        .setReportDelay(ScanSettings.AUTO_BATCH_MIN_REPORT_DELAY_MILLIS).build());
        PendingIntent rejectedIntent = new PendingIntent(new android.content.IIntentSender.Stub() {
            @Override public void send(int code, Intent intent, String type, android.os.IBinder allowlist,
                    android.content.IIntentReceiver finished, String permission, android.os.Bundle options) {
                throw new AssertionError("An unsupported scan must not send its PendingIntent");
            }
        });
        for (ScanSettings settings : unsupported) {
            scanner.startScan(batteryFilter, settings, battery);
            check(take(failures, "unsupported scan settings") == ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED,
                    "unsupported settings fail before registration");
            check(take(failureThreads, "scan failure dispatch") != Thread.currentThread(),
                    "scan failure is posted to the callback handler");
            check(failures.poll(100, TimeUnit.MILLISECONDS) == null && matched.isEmpty()
                    && unexpectedReports.isEmpty(), "one error and no scan reports for rejected settings");
            check(scanner.startScan(batteryFilter, settings, rejectedIntent)
                    == ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED,
                    "PendingIntent reports unsupported settings synchronously");
        }
        // The same callback remains available after each rejection.
        scanner.startScan(batteryFilter, fast, battery);
        scanner.startScan(List.of(new ScanFilter.Builder().setServiceUuid(
                ParcelUuid.fromString("0000180d-0000-1000-8000-00805f9b34fb")).build()), fast, heartRate);
        ScanResult merged = null;
        long end = System.currentTimeMillis() + 5000;
        while (merged == null && System.currentTimeMillis() < end) {
            ScanResult r = matched.poll(1, TimeUnit.SECONDS);
            // The advertisement merged with the scan response that follows it.
            if (r != null && r.getScanRecord() != null && r.getScanRecord().getTxPowerLevel() == -4) merged = r;
        }
        check(merged != null, "a scan result with the advertisement and its scan response");
        check(PERIPHERAL.equals(merged.getDevice().getAddress())
                && merged.getDevice().getAddressType() == BluetoothDevice.ADDRESS_TYPE_RANDOM, "random address");
        check("Gesher BLE".equals(merged.getScanRecord().getDeviceName()), "local name");
        check(merged.getRssi() == -50 && merged.isConnectable() && merged.isLegacy(), "RSSI, connectable, legacy");
        byte[] maker = merged.getScanRecord().getManufacturerSpecificData(0x1234);
        check(maker != null && Arrays.equals(maker, new byte[] {1, 2}), "manufacturer data 0x1234");
        check(merged.getDevice().getType() == BluetoothDevice.DEVICE_TYPE_LE
                && "Gesher BLE".equals(merged.getDevice().getName()), "the advertiser is known as an LE device");
        matched.clear();
        scanner.startScan(batteryFilter, unsupported.get(0), battery);
        check(failures.poll(5, TimeUnit.SECONDS) == ScanCallback.SCAN_FAILED_ALREADY_STARTED,
                "duplicate callback takes precedence over unsupported replacement settings");
        take(matched, "the original scan after a duplicate rejection");
        check(failures.poll(100, TimeUnit.MILLISECONDS) == null && unexpectedReports.isEmpty(),
                "duplicate rejection leaves the original immediate scan active");
        scanner.stopScan(battery);
        scanner.stopScan(heartRate);
        check(other.isEmpty(), "a filter for another service matched nothing");
        LinkedBlockingQueue<byte[]> legacy = new LinkedBlockingQueue<>();
        BluetoothAdapter.LeScanCallback le = (device, rssi, record) -> legacy.add(record);
        check(adapter.startLeScan(new UUID[] {BATTERY}, le), "legacy startLeScan");
        check(take(legacy, "a legacy scan record").length > 0, "legacy scan record bytes");
        adapter.stopLeScan(le);
        step("LE scan: no-offload settings errors and retry, filtered merged results, legacy startLeScan");
    }

    private static void gatt(BluetoothAdapter adapter, Context app) throws Exception {
        LinkedBlockingQueue<String> events = new LinkedBlockingQueue<>();
        BluetoothGattCallback callback = new BluetoothGattCallback() {
            @Override
            public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
                events.add("state " + status + " " + state);
            }

            @Override
            public void onServicesDiscovered(BluetoothGatt g, int status) {
                events.add("discovered " + status);
            }

            @Override
            public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v, int status) {
                events.add("read " + status + " " + Arrays.toString(v));
            }

            @Override
            public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
                events.add("write " + status);
            }

            @Override
            public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
                events.add("descriptor " + status);
            }

            @Override
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
                events.add("changed " + c.getUuid() + " " + new String(v));
            }

            @Override
            public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
                events.add("mtu " + mtu + " " + status);
            }

            @Override
            public void onPhyRead(BluetoothGatt g, int tx, int rx, int status) {
                events.add("phy-read " + tx + " " + rx + " " + status);
            }

            @Override
            public void onPhyUpdate(BluetoothGatt g, int tx, int rx, int status) {
                events.add("phy-update " + tx + " " + rx + " " + status);
            }
        };
        BluetoothGatt unknown = adapter.getRemoteDevice("11:22:33:44:55:66").connectGatt(app, false, callback);
        check(take(events, "a failed connection").equals("state 133 0"), "an unknown device: status 133");
        unknown.close();

        BluetoothDevice device = adapter.getRemoteLeDevice(PERIPHERAL, BluetoothDevice.ADDRESS_TYPE_RANDOM);
        BluetoothGatt gatt = device.connectGatt(app, false, callback, BluetoothDevice.TRANSPORT_LE);
        check(take(events, "connected").equals("state 0 2"), "connected");
        BluetoothManager manager = new BluetoothManager(app);
        check(manager.getConnectedDevices(BluetoothProfile.GATT).contains(device), "listed as a GATT connection");
        check(gatt.discoverServices(), "discoverServices");
        check(take(events, "services").equals("discovered 0"), "services discovered");
        List<BluetoothGattService> services = gatt.getServices();
        check(services.size() == 4, "three primary services and a secondary one: " + services.size());
        BluetoothGattService uartService = gatt.getService(UART);
        List<BluetoothGattService> included = uartService.getIncludedServices();
        check(uartService.getType() == BluetoothGattService.SERVICE_TYPE_PRIMARY && included.size() == 1
                && included.get(0).getType() == BluetoothGattService.SERVICE_TYPE_SECONDARY
                && included.get(0).getCharacteristics().size() == 1, "UART includes a secondary service");
        BluetoothGattCharacteristic level = gatt.getService(BATTERY).getCharacteristic(LEVEL);
        check(level != null && gatt.readCharacteristic(level), "read the battery level");
        check(take(events, "the read").equals("read 0 [87]"), "battery level 87");
        java.lang.reflect.Method byUuid = BluetoothGatt.class.getMethod("readUsingCharacteristicUuid",
                UUID.class, int.class, int.class);
        check(!(Boolean) byUuid.invoke(gatt, LEVEL, 20, 40), "no battery level in handles 20-40");
        check((Boolean) byUuid.invoke(gatt, LEVEL, 1, 0xffff), "read by UUID");
        check(take(events, "the read by UUID").equals("read 0 [87]"), "read by UUID: 87");
        check(gatt.setCharacteristicNotification(level, true), "registered for notifications");
        BluetoothGattDescriptor cccd = level.getDescriptor(CCCD);
        check(cccd != null && gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                == android.bluetooth.BluetoothStatusCodes.SUCCESS, "subscribe through the CCCD");
        Set<String> got = new HashSet<>();
        got.add(take(events, "the CCCD write"));
        got.add(take(events, "the battery notification"));
        check(got.contains("descriptor 0") && got.contains("changed " + LEVEL + " " + new String(new byte[] {86})),
                "CCCD written, battery notification 86: " + got);
        BluetoothGattService uart = gatt.getService(UART);
        BluetoothGattCharacteristic tx = uart.getCharacteristic(TX);
        gatt.setCharacteristicNotification(tx, true);
        check(gatt.writeDescriptor(tx.getDescriptor(CCCD), BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                == android.bluetooth.BluetoothStatusCodes.SUCCESS, "subscribe to TX");
        check(take(events, "the TX CCCD").equals("descriptor 0"), "TX CCCD written");
        BluetoothGattCharacteristic rx = uart.getCharacteristic(RX);
        check(gatt.writeCharacteristic(rx, "hello".getBytes(), BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
                == android.bluetooth.BluetoothStatusCodes.SUCCESS, "write RX");
        got.clear();
        got.add(take(events, "the RX write"));
        got.add(take(events, "the echo"));
        check(got.contains("write 0") && got.contains("changed " + TX + " hello"), "RX echoed on TX: " + got);
        check(gatt.requestMtu(517), "requestMtu");
        check(take(events, "the MTU").equals("mtu 247 0"), "MTU 247");
        gatt.readPhy();
        check(take(events, "the PHY").equals("phy-read 2 2 0"), "readPhy: what Windows reports (2M)");
        gatt.setPreferredPhy(BluetoothDevice.PHY_LE_CODED_MASK, BluetoothDevice.PHY_LE_CODED_MASK, 0);
        check(take(events, "the PHY update").equals("phy-update 2 2 " + BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED),
                "setPreferredPhy is declined, the PHY unchanged");
        LinkedBlockingQueue<String> second = new LinkedBlockingQueue<>();
        BluetoothGatt other = device.connectGatt(app, false, new BluetoothGattCallback() {
            @Override
            public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
                second.add("state " + status + " " + state);
            }
        });
        check(take(second, "the second client").equals("state 0 2"), "a second client to the same device");
        other.close();
        check(manager.getConnectedDevices(BluetoothProfile.GATT).contains(device),
                "closing one client keeps the device connected for the other");
        gatt.disconnect();
        check(take(events, "disconnected").equals("state 0 0"), "disconnected");
        check(!manager.getConnectedDevices(BluetoothProfile.GATT).contains(device), "no longer connected");
        gatt.close();
        step("GATT: connect, discover 3 services and an included secondary one, read 87 (by UUID too), notifications through the CCCD, write/echo, MTU 247, PHY, two clients, disconnect");
    }

    private static void rfcomm(BluetoothAdapter adapter) throws Exception {
        BluetoothDevice headset = adapter.getRemoteDevice(HEADSET);
        BluetoothSocket socket = headset.createRfcommSocketToServiceRecord(VENDOR);
        check(!socket.isConnected(), "not connected before connect()");
        socket.connect();
        check(socket.isConnected() && socket.getConnectionType() == BluetoothSocket.TYPE_RFCOMM, "connected");
        byte[] packet = {0x08, (byte) 0xEE, 0x00, 0x00, 0x00, 0x01, 0x01, 0x0A, 0x00, 0x02};
        OutputStream out = socket.getOutputStream();
        InputStream in = socket.getInputStream();
        out.write(packet);
        out.flush();
        long end = System.currentTimeMillis() + 5000;
        while (in.available() < packet.length && System.currentTimeMillis() < end) Thread.sleep(10);
        check(in.available() == packet.length, "available() counts the echo");
        byte[] back = new byte[packet.length];
        int n = 0;
        while (n < back.length) n += in.read(back, n, back.length - n);
        check(Arrays.equals(back, packet), "the vendor service echoes a packet");
        java.lang.reflect.Method encrypted = BluetoothDevice.class.getMethod("isEncrypted");
        check((Boolean) encrypted.invoke(headset), "a secure socket's link is encrypted");
        socket.close();
        check(!(Boolean) encrypted.invoke(headset), "no longer once it is closed");
        BluetoothSocket slow = (BluetoothSocket) BluetoothDevice.class
                .getMethod("createInsecureRfcommSocket", int.class).invoke(headset, 3);
        Thread closer = new Thread(() -> {
            try {
                Thread.sleep(300);
                slow.close();
            } catch (Exception ignored) {
                // the check below fails
            }
        });
        closer.start();
        long start = System.currentTimeMillis();
        try {
            slow.connect();
            check(false, "a connect that never answers returned");
        } catch (IOException expected) {
            check(System.currentTimeMillis() - start < 5000, "close() cancels a blocked connect");
        }
        closer.join();
        try {
            in.read();
            check(false, "read after close");
        } catch (IOException expected) {
            checks++;
        }
        try {
            headset.createInsecureRfcommSocketToServiceRecord(UUID.randomUUID()).connect();
            check(false, "an absent service connects");
        } catch (IOException expected) {
            checks++;
        }
        BluetoothSocket channel = (BluetoothSocket) BluetoothDevice.class
                .getMethod("createRfcommSocket", int.class).invoke(headset, 1);
        channel.connect();
        check(channel.isConnected(), "createRfcommSocket(1) through reflection: SPP on channel 1");
        channel.close();

        UUID chat = UUID.fromString("fa87c0d0-afac-11de-8a39-0800200c9a66");
        BluetoothServerSocket server = adapter.listenUsingRfcommWithServiceRecord("Gesher chat", chat);
        BluetoothSocket client = adapter.getRemoteDevice("00:1A:7D:DA:71:13").createRfcommSocketToServiceRecord(chat);
        client.connect();
        BluetoothSocket accepted = server.accept(5000);
        client.getOutputStream().write(0x5A);
        check(accepted.getInputStream().read() == 0x5A, "the listener's connection reads what the client wrote");
        accepted.getOutputStream().write(0x33);
        check(client.getInputStream().read() == 0x33, "and the client reads the reply");
        try {
            server.accept(300);
            check(false, "accept times out");
        } catch (IOException expected) {
            checks++;
        }
        client.close();
        accepted.close();
        server.close();
        step("RFCOMM: vendor service echo, available(), encrypted secure link, close() cancels a connect, refused service, createRfcommSocket(1), listener accept and exchange");
    }

    private static List<String> actions(LinkedBlockingQueue<Intent> broadcasts, int count) throws Exception {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < count; i++) {
            Intent intent = broadcasts.poll(10, TimeUnit.SECONDS);
            if (intent == null) break;
            String text = intent.getAction();
            if (intent.hasExtra(BluetoothAdapter.EXTRA_STATE)) text += " " + intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1);
            if (intent.hasExtra(BluetoothProfile.EXTRA_STATE)) text += " " + intent.getIntExtra(BluetoothProfile.EXTRA_STATE, -1);
            if (intent.hasExtra(BluetoothDevice.EXTRA_NAME)) text += " " + intent.getStringExtra(BluetoothDevice.EXTRA_NAME);
            if (intent.hasExtra(BluetoothDevice.EXTRA_BOND_STATE)) {
                BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                text += " " + d + " " + intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1)
                        + ">" + intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1);
            }
            out.add(text);
        }
        return out;
    }

    private static void broadcastsAndDiscovery(BluetoothAdapter adapter, BluetoothManagerService service,
            LinkedBlockingQueue<Intent> broadcasts) throws Exception {
        service.poll();  // the baseline: no broadcast
        broadcasts.clear();
        // An app targeting API 30 may still turn the radio off and on.
        BluetoothAdapter legacy = new BluetoothManager(new App(30, "android.permission.BLUETOOTH",
                "android.permission.BLUETOOTH_ADMIN")).getAdapter();
        check(!adapter.disable(), "API 34 apps cannot turn the radio off");
        check(legacy.disable(), "API 30 app turns the radio off");
        List<String> off = actions(broadcasts, 6);
        step("off: " + off);
        check(off.contains(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_TURNING_OFF)
                && off.contains(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_OFF)
                && off.contains(BluetoothDevice.ACTION_ACL_DISCONNECTED)
                && off.contains(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED + " 0"),
                "TURNING_OFF, OFF, ACL_DISCONNECTED and A2DP disconnected");
        check(off.indexOf(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_TURNING_OFF)
                < off.indexOf(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_OFF), "in order");
        check(!adapter.isEnabled() && adapter.getBondedDevices().isEmpty()
                && adapter.getBluetoothLeScanner() == null, "off: disabled, no bonded devices, no scanner");
        check(legacy.enable(), "and on again");
        List<String> on = actions(broadcasts, 6);
        step("on: " + on);
        check(on.contains(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_TURNING_ON)
                && on.contains(BluetoothAdapter.ACTION_STATE_CHANGED + " " + BluetoothAdapter.STATE_ON)
                && on.contains(BluetoothDevice.ACTION_ACL_CONNECTED)
                && !on.contains(BluetoothDevice.ACTION_BOND_STATE_CHANGED), "TURNING_ON, ON, ACL_CONNECTED; the bond is kept");
        broadcasts.clear();
        check(adapter.startDiscovery(), "startDiscovery");
        List<String> discovery = actions(broadcasts, 4);
        step("discovery: " + discovery);
        check(discovery.equals(List.of(BluetoothAdapter.ACTION_DISCOVERY_STARTED,
                BluetoothDevice.ACTION_FOUND + " Gesher Test Headset",
                BluetoothDevice.ACTION_FOUND + " Gesher Test Speaker",
                BluetoothAdapter.ACTION_DISCOVERY_FINISHED)), "discovery broadcasts");
        check(adapter.getRemoteDevice(HEADSET).fetchUuidsWithSdp(), "fetchUuidsWithSdp");
        Intent uuid = broadcasts.poll(10, TimeUnit.SECONDS);
        check(uuid != null && BluetoothDevice.ACTION_UUID.equals(uuid.getAction()), "ACTION_UUID");
        Parcelable[] list = uuid.getParcelableArrayExtra(BluetoothDevice.EXTRA_UUID);
        check(list != null && list.length == 4, "four service classes in ACTION_UUID");
        // Pairing through Windows (the test radio accepts the speaker as if the user did).
        broadcasts.clear();
        BluetoothDevice speaker = adapter.getRemoteDevice("00:02:5B:00:B0:B0");
        check(!adapter.getRemoteDevice(HEADSET).createBond(), "a bonded device does not pair again");
        check(speaker.createBond(), "createBond starts pairing");
        String bonding = BluetoothDevice.ACTION_BOND_STATE_CHANGED + " 00:02:5B:00:B0:B0 ";
        List<String> paired = actions(broadcasts, 2);
        step("pairing: " + paired);
        check(paired.equals(List.of(bonding + BluetoothDevice.BOND_NONE + ">" + BluetoothDevice.BOND_BONDING,
                bonding + BluetoothDevice.BOND_BONDING + ">" + BluetoothDevice.BOND_BONDED)),
                "BOND_STATE_CHANGED: NONE>BONDING, BONDING>BONDED");
        Thread.sleep(2100);  // the process's paired-device cache
        check(adapter.getBondedDevices().contains(speaker) && speaker.getBondState() == BluetoothDevice.BOND_BONDED,
                "the speaker is bonded");
        check((Boolean) BluetoothDevice.class.getMethod("removeBond").invoke(speaker), "removeBond (reflection)");
        List<String> unpaired = actions(broadcasts, 1);
        check(unpaired.equals(List.of(bonding + BluetoothDevice.BOND_BONDED + ">" + BluetoothDevice.BOND_NONE)),
                "BOND_STATE_CHANGED: BONDED>NONE " + unpaired);
        check(adapter.getRemoteDevice("11:22:33:44:55:66").createBond(), "pairing an absent device starts");
        String other = BluetoothDevice.ACTION_BOND_STATE_CHANGED + " 11:22:33:44:55:66 ";
        check(actions(broadcasts, 2).equals(List.of(other + BluetoothDevice.BOND_NONE + ">" + BluetoothDevice.BOND_BONDING,
                other + BluetoothDevice.BOND_BONDING + ">" + BluetoothDevice.BOND_NONE)), "and fails: BONDING>NONE");
        step("broadcasts: state, ACL and A2DP changes, discovery (FOUND x2), ACTION_UUID, pairing and unpairing");
    }
}
