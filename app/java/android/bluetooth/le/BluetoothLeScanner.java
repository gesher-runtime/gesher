/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth.le;

import android.app.PendingIntent;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothHost;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import com.gesher.server.WindowsBluetooth;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * BluetoothLeScanner with the Bluetooth module's public API over Windows'
 * BluetoothLEAdvertisementWatcher ({@link WindowsBluetooth}; docs/bluetooth.md). One active
 * watcher serves every scan of the process; each scan gets the reports its filters match,
 * with the advertisement and the last scan response of the device merged into one
 * ScanRecord (as Android reports legacy advertisements). Without controller-offloaded
 * filtering, Android permits only ALL_MATCHES with a zero report delay. Other settings
 * fail with SCAN_FAILED_FEATURE_UNSUPPORTED; callbacks come on the main thread.
 *
 * <p>Results reach the app only with the location permission the module asks for
 * ({@code neverForLocation} on API 31+ exempts it); without it the scan runs and stays silent.
 */
public final class BluetoothLeScanner {
    private static final String TAG = "BluetoothLeScanner";

    public static final String EXTRA_LIST_SCAN_RESULT =
            "android.bluetooth.le.extra.LIST_SCAN_RESULT";
    public static final String EXTRA_ERROR_CODE = "android.bluetooth.le.extra.ERROR_CODE";
    public static final String EXTRA_CALLBACK_TYPE = "android.bluetooth.le.extra.CALLBACK_TYPE";

    /** A device that has not advertised for this long is lost (CALLBACK_TYPE_MATCH_LOST). */
    static final long LOST_AFTER_MS = 10_000;

    // Like Android's controller buffer, a full batch is delivered before the
    // requested delay. A very long report delay must not retain every report.
    static final int MAX_BATCH_REPORTS = 512;

    private final Context mContext;

    /** @hide */
    public BluetoothLeScanner(Context context) {
        mContext = context;
    }

    // --- the process's scans ------------------------------------------------------------------

    private static final class Client {
        final Object key;  // the ScanCallback or PendingIntent
        final ScanCallback callback;
        final PendingIntent intent;
        final Context context;
        final List<ScanFilter> filters;
        final ScanSettings settings;
        final boolean mayReceive;
        final Set<String> found = new HashSet<>();
        final Map<String, ScanResult> lastSeen = new HashMap<>();
        final List<ScanResult> batch = new ArrayList<>();
        long batchAt = SystemClock.elapsedRealtime();

        Client(Object key, ScanCallback callback, PendingIntent intent, Context context,
                List<ScanFilter> filters, ScanSettings settings) {
            this.key = key;
            this.callback = callback;
            this.intent = intent;
            this.context = context;
            this.filters = filters == null ? new ArrayList<>() : new ArrayList<>(filters);
            this.settings = settings;
            this.mayReceive = BluetoothHost.mayReceiveScanResults(context);
        }

        boolean matches(ScanResult result) {
            if (filters.isEmpty()) return true;
            for (ScanFilter f : filters) {
                if (f == null || f.matches(result)) return true;
            }
            return false;
        }

        int callbackType() {
            return settings.getCallbackType();
        }
    }

    private static final Object sLock = new Object();
    private static final Map<Object, Client> sClients = new LinkedHashMap<>();
    private static final Map<String, byte[]> sAdvertisements = bounded();
    private static final Map<String, byte[]> sResponses = bounded();
    private static final Map<String, Integer> sRssi = bounded();
    private static Thread sThread;
    private static Handler sHandler;

    /** Per-device memory, bounded (random addresses come and go). */
    private static <V> Map<String, V> bounded() {
        return new LinkedHashMap<String, V>(64, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<String, V> eldest) {
                return size() > 512;
            }
        };
    }

    /** The RSSI of the last advertisement of {@code address} this process saw, or null. */
    public static Integer lastRssi(String address) {
        synchronized (sLock) {
            return sRssi.get(address);
        }
    }

    private static Handler handler() {
        synchronized (sLock) {
            if (sHandler == null) {
                Looper main = Looper.getMainLooper();
                if (main == null) {
                    HandlerThread t = new HandlerThread("gesher-ble-callbacks");
                    t.start();
                    main = t.getLooper();
                }
                sHandler = new Handler(main);
            }
            return sHandler;
        }
    }

    private static void startHub() {
        // Called with sLock held: one watcher for every scan of the process.
        if (sThread != null) return;
        sThread = new Thread(BluetoothLeScanner::hub, "gesher-ble-scan");
        sThread.setDaemon(true);
        sThread.start();
    }

    private static void hub() {
        int[] hr = new int[1];
        long scan = WindowsBluetooth.nativeScanStart(true, hr);
        if (scan == 0) {
            Log.w(TAG, "LE scan failed to start: " + WindowsBluetooth.hresult(hr[0]));
            failAll(ScanCallback.SCAN_FAILED_INTERNAL_ERROR);
            return;
        }
        Log.i(TAG, "LE scan started");
        long[] meta = new long[6];
        long housekeepingAt = 0;
        try {
            for (;;) {
                synchronized (sLock) {
                    if (sClients.isEmpty()) {
                        sThread = null;
                        return;
                    }
                }
                meta[5] = 0;
                byte[] data = WindowsBluetooth.nativeScanNext(scan, 200, meta);
                if (data != null) {
                    deliver(result(data, meta));
                } else if (meta[5] != 0) {
                    Log.w(TAG, "Windows ended the LE scan: " + WindowsBluetooth.hresult((int) meta[5]));
                    failAll(ScanCallback.SCAN_FAILED_INTERNAL_ERROR);
                    return;
                }
                long now = SystemClock.elapsedRealtime();
                if (now - housekeepingAt >= 200) {
                    housekeepingAt = now;
                    housekeeping(now);
                }
            }
        } finally {
            WindowsBluetooth.nativeScanStop(scan);
            Log.i(TAG, "LE scan stopped");
        }
    }

    private static ScanResult result(byte[] data, long[] meta) {
        String address = WindowsBluetooth.address(meta[0]);
        int flags = (int) meta[4];
        boolean connectable = (flags & 1) != 0;
        boolean scanResponse = (flags & 2) != 0;
        boolean legacy = (flags & 4) != 0;
        byte[] merged;
        synchronized (sLock) {
            sRssi.put(address, (int) meta[2]);
            byte[] adv;
            byte[] response;
            if (scanResponse) {
                // Android reports a scan response together with the advertisement it follows.
                sResponses.put(address, data);
                adv = sAdvertisements.get(address);
                response = data;
            } else {
                sAdvertisements.put(address, data);
                adv = data;
                response = sResponses.get(address);
            }
            if (adv == null) adv = new byte[0];
            if (response == null) response = new byte[0];
            merged = new byte[adv.length + response.length];
            System.arraycopy(adv, 0, merged, 0, adv.length);
            System.arraycopy(response, 0, merged, adv.length, response.length);
        }
        ScanRecord record = ScanRecord.parseFromBytes(merged);
        BluetoothDevice device = BluetoothDevice.createFromHost(address, (int) meta[1]);
        BluetoothHost.sawLe(address, record == null ? null : record.getDeviceName());
        int eventType = (connectable ? 0x01 : 0) | (scanResponse ? 0x08 : 0) | (legacy ? 0x10 : 0);
        return new ScanResult(device, eventType, BluetoothDevice.PHY_LE_1M, ScanResult.PHY_UNUSED,
                ScanResult.SID_NOT_PRESENT, (int) meta[3], (int) meta[2],
                ScanResult.PERIODIC_INTERVAL_NOT_PRESENT, record, SystemClock.elapsedRealtimeNanos());
    }

    private static void deliver(ScanResult result) {
        String address = result.getDevice().getAddress();
        List<Runnable> calls = new ArrayList<>();
        synchronized (sLock) {
            for (Client c : sClients.values()) {
                if (!c.mayReceive || !c.matches(result)) continue;
                ScanResult forClient = withContext(result, c.context);
                int type = c.callbackType();
                if ((type & (ScanSettings.CALLBACK_TYPE_FIRST_MATCH
                        | ScanSettings.CALLBACK_TYPE_MATCH_LOST)) != 0) {
                    c.lastSeen.put(address, forClient);
                }
                if (c.settings.getReportDelayMillis() > 0) {
                    c.batch.add(forClient);
                    if (c.batch.size() >= MAX_BATCH_REPORTS) {
                        List<ScanResult> results = new ArrayList<>(c.batch);
                        c.batch.clear();
                        c.batchAt = SystemClock.elapsedRealtime();
                        calls.add(() -> batch(c, results));
                    }
                    continue;
                }
                if ((type & ScanSettings.CALLBACK_TYPE_ALL_MATCHES) != 0
                        || (type & ScanSettings.CALLBACK_TYPE_ALL_MATCHES_AUTO_BATCH) != 0) {
                    calls.add(() -> report(c, ScanSettings.CALLBACK_TYPE_ALL_MATCHES, forClient));
                }
                if ((type & ScanSettings.CALLBACK_TYPE_FIRST_MATCH) != 0 && c.found.add(address)) {
                    calls.add(() -> report(c, ScanSettings.CALLBACK_TYPE_FIRST_MATCH, forClient));
                }
            }
        }
        Handler h = handler();
        for (Runnable r : calls) h.post(r);
    }

    private static ScanResult withContext(ScanResult r, Context context) {
        BluetoothDevice d = BluetoothHost.device(r.getDevice().getAddress(),
                r.getDevice().getAddressType(), context);
        return new ScanResult(d, eventType(r), r.getPrimaryPhy(), r.getSecondaryPhy(),
                r.getAdvertisingSid(), r.getTxPower(), r.getRssi(),
                r.getPeriodicAdvertisingInterval(), r.getScanRecord(), r.getTimestampNanos());
    }

    private static int eventType(ScanResult r) {
        return (r.isConnectable() ? 0x01 : 0) | (r.isLegacy() ? 0x10 : 0);
    }

    /** Batches whose delay passed, and devices not seen for LOST_AFTER_MS. */
    private static void housekeeping(long now) {
        List<Runnable> calls = new ArrayList<>();
        long lostBefore = SystemClock.elapsedRealtimeNanos() - LOST_AFTER_MS * 1_000_000L;
        synchronized (sLock) {
            for (Client c : sClients.values()) {
                long delay = c.settings.getReportDelayMillis();
                if (delay > 0 && now - c.batchAt >= delay) {
                    c.batchAt = now;
                    if (!c.batch.isEmpty()) {
                        List<ScanResult> results = new ArrayList<>(c.batch);
                        c.batch.clear();
                        calls.add(() -> batch(c, results));
                    }
                }
                // FIRST_MATCH-only scans also forget a device after it leaves,
                // so a later arrival can produce a new first-match callback.
                if (!c.lastSeen.isEmpty()) {
                    for (Iterator<Map.Entry<String, ScanResult>> it = c.lastSeen.entrySet().iterator();
                            it.hasNext();) {
                        Map.Entry<String, ScanResult> e = it.next();
                        if (e.getValue().getTimestampNanos() < lostBefore) {
                            ScanResult lost = e.getValue();
                            it.remove();
                            c.found.remove(e.getKey());
                            if ((c.callbackType() & ScanSettings.CALLBACK_TYPE_MATCH_LOST) != 0) {
                                calls.add(() -> report(c, ScanSettings.CALLBACK_TYPE_MATCH_LOST, lost));
                            }
                        }
                    }
                }
            }
        }
        Handler h = handler();
        for (Runnable r : calls) h.post(r);
    }

    private static boolean active(Client c) {
        synchronized (sLock) {
            return sClients.get(c.key) == c;
        }
    }

    private static void report(Client c, int callbackType, ScanResult result) {
        if (!active(c)) return;
        if (c.callback != null) {
            c.callback.onScanResult(callbackType, result);
            return;
        }
        ArrayList<ScanResult> list = new ArrayList<>();
        list.add(result);
        sendIntent(c, callbackType, list, 0);
    }

    private static void batch(Client c, List<ScanResult> results) {
        if (!active(c)) return;
        if (c.callback != null) {
            c.callback.onBatchScanResults(results);
        } else {
            sendIntent(c, ScanSettings.CALLBACK_TYPE_ALL_MATCHES, new ArrayList<>(results), 0);
        }
    }

    private static void sendIntent(Client c, int callbackType, ArrayList<ScanResult> results,
            int error) {
        Intent fill = new Intent();
        if (error != 0) {
            fill.putExtra(EXTRA_ERROR_CODE, error);
        } else {
            fill.putExtra(EXTRA_CALLBACK_TYPE, callbackType);
            fill.putParcelableArrayListExtra(EXTRA_LIST_SCAN_RESULT, results);
        }
        try {
            c.intent.send(c.context, 0, fill);
        } catch (PendingIntent.CanceledException e) {
            synchronized (sLock) {
                sClients.remove(c.key);
            }
        }
    }

    private static void failAll(int error) {
        List<Client> failed;
        synchronized (sLock) {
            failed = new ArrayList<>(sClients.values());
            sClients.clear();
            sThread = null;
        }
        Handler h = handler();
        for (Client c : failed) {
            h.post(() -> {
                if (c.callback != null) {
                    c.callback.onScanFailed(error);
                } else {
                    sendIntent(c, 0, null, error);
                }
            });
        }
    }

    // --- the API ---------------------------------------------------------------------------

    public void startScan(ScanCallback callback) {
        startScan(null, new ScanSettings.Builder().build(), callback);
    }

    public void startScan(List<ScanFilter> filters, ScanSettings settings,
            ScanCallback callback) {
        start(filters, settings, callback, null);
    }

    public int startScan(List<ScanFilter> filters, ScanSettings settings,
            PendingIntent callbackIntent) {
        return start(filters, settings != null ? settings : new ScanSettings.Builder().build(),
                null, callbackIntent);
    }

    private int start(List<ScanFilter> filters, ScanSettings settings, ScanCallback callback,
            PendingIntent intent) {
        if (callback == null && intent == null) throw new IllegalArgumentException("callback is null");
        if (settings == null) throw new IllegalArgumentException("settings is null");
        if (!BluetoothHost.on()) {
            Log.w(TAG, "BLE is not available");
            return error(callback, ScanCallback.SCAN_FAILED_INTERNAL_ERROR);
        }
        Context context = BluetoothHost.context(mContext);
        Object key = callback != null ? callback : intent;
        synchronized (sLock) {
            if (callback != null && sClients.containsKey(key)) {
                return error(callback, ScanCallback.SCAN_FAILED_ALREADY_STARTED);
            }
            // Android 16 BluetoothLeScanner.isSettingsConfigAllowedForScan:
            // no offloaded filtering means only immediate ALL_MATCHES scans.
            // Software batching/tracking is not a substitute for that capability.
            if (settings.getCallbackType() != ScanSettings.CALLBACK_TYPE_ALL_MATCHES
                    || settings.getReportDelayMillis() != 0) {
                return error(callback, ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED);
            }
            // Android performs these local admission checks before the service
            // enforces scan permission when registering an accepted request.
            BluetoothHost.enforceScan(mContext);
            Client c = new Client(key, callback, intent, context, filters, settings);
            if (!c.mayReceive) {
                Log.w(TAG, "scan results withheld: no location permission (and no neverForLocation)");
            }
            sClients.put(key, c);
            startHub();
        }
        return ScanCallback.NO_ERROR;
    }

    private static int error(ScanCallback callback, int code) {
        if (callback == null) return code;
        handler().post(() -> callback.onScanFailed(code));
        return ScanCallback.NO_ERROR;
    }

    public void stopScan(ScanCallback callback) {
        BluetoothHost.enforceScan(mContext);
        synchronized (sLock) {
            sClients.remove(callback);
        }
    }

    public void stopScan(PendingIntent callbackIntent) {
        BluetoothHost.enforceScan(mContext);
        synchronized (sLock) {
            sClients.remove(callbackIntent);
        }
    }

    /** Delivers a scan's batched results now. */
    public void flushPendingScanResults(ScanCallback callback) {
        if (callback == null) throw new IllegalArgumentException("callback cannot be null!");
        BluetoothHost.enforceScan(mContext);
        List<ScanResult> results = null;
        Client client;
        synchronized (sLock) {
            client = sClients.get(callback);
            if (client != null && !client.batch.isEmpty()) {
                results = new ArrayList<>(client.batch);
                client.batch.clear();
                client.batchAt = SystemClock.elapsedRealtime();
            }
        }
        if (client != null && results != null) {
            final List<ScanResult> flushed = results;
            handler().post(() -> batch(client, flushed));
        }
    }

    /** @hide */
    public void cleanup() {
        synchronized (sLock) {
            sClients.clear();
        }
    }
}
