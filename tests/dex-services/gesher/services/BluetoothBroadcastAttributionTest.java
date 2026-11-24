/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.le.ScanResult;
import android.content.AttributionSource;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Issues #303 and #418: a registered receiver of a Bluetooth broadcast carrying EXTRA_DEVICE. The
 * real LoadedApk.ReceiverDispatcher runs framework.jar's Intent.prepareToEnterProcess,
 * which calls the hidden BluetoothDevice.prepareToEnterProcess(AttributionSource) on
 * gesher-app.jar's BluetoothDevice before onReceive. The intent crosses a Parcel first, as
 * a broadcast from the system server does.
 */
public final class BluetoothBroadcastAttributionTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static final class AppThread extends IApplicationThread.Default {
        private final Binder mBinder = new Binder();
        @Override public IBinder asBinder() { return mBinder; }
    }

    static Context systemContext(String name) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap(name, root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.createInProcess(List.of()));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        return (Context) create.invoke(null, constructor.newInstance());
    }

    private static Intent roundTrip(Intent original) {
        Parcel parcel = Parcel.obtain();
        try {
            original.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return Intent.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }

    private static BluetoothDevice receive(Context context, Intent delivered) throws Exception {
        HandlerThread thread = new HandlerThread("bluetooth-receiver");
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<BluetoothDevice> received = new AtomicReference<>();
        CountDownLatch done = new CountDownLatch(1);
        thread.setUncaughtExceptionHandler((t, e) -> {
            failure.set(e);
            done.countDown();
        });
        thread.start();
        try {
            BroadcastReceiver receiver = new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent intent) {
                    received.set(intent.getParcelableExtra(
                            BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class));
                    done.countDown();
                }
            };
            Class<?> dispatcherType = Class.forName("android.app.LoadedApk$ReceiverDispatcher");
            Constructor<?> make = dispatcherType.getDeclaredConstructor(IApplicationThread.class,
                    BroadcastReceiver.class, Context.class, Handler.class,
                    Class.forName("android.app.Instrumentation"), boolean.class);
            make.setAccessible(true);
            Object dispatcher = make.newInstance(new AppThread(), receiver, context,
                    new Handler(thread.getLooper()), null, true);
            Method perform = dispatcherType.getDeclaredMethod("performReceive", Intent.class, int.class,
                    String.class, android.os.Bundle.class, boolean.class, boolean.class, boolean.class,
                    int.class, int.class, String.class);
            perform.setAccessible(true);
            perform.invoke(dispatcher, delivered, 0, null, null, false, false, true, 0,
                    Process.SYSTEM_UID, "android");

            require(done.await(30, TimeUnit.SECONDS), "Receiver dispatch did not finish");
            if (failure.get() != null) {
                throw new AssertionError("Receiver dispatch failed before onReceive", failure.get());
            }
        } finally {
            thread.quitSafely();
            thread.join(5000);
        }
        require(!thread.isAlive(), "Receiver thread did not terminate");
        return received.get();
    }

    public static void main(String[] args) throws Exception {
        Context context = systemContext("bluetooth-broadcast-test");
        require(BluetoothDevice.class.getClassLoader() == Object.class.getClassLoader(),
                "BluetoothDevice is not the boot class path's (gesher-app.jar) class");
        Field attribution = BluetoothDevice.class.getDeclaredField("mAttributionSource");
        attribution.setAccessible(true);
        AttributionSource source = context.getAttributionSource();

        BluetoothDevice sent = BluetoothDevice.createFromHost("00:11:22:33:44:55",
                BluetoothDevice.ADDRESS_TYPE_PUBLIC);
        sent.setAttributionSource(source);
        Intent original = new Intent(BluetoothDevice.ACTION_FOUND);
        original.putExtra(BluetoothDevice.EXTRA_DEVICE, sent);
        BluetoothDevice beforeDispatch = roundTrip(original).getParcelableExtra(
                BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class);
        require(beforeDispatch != null, "Unparceled intent missing EXTRA_DEVICE");
        require(attribution.get(beforeDispatch) == null,
                "Device attribution must not be transported in the parcel");

        // Keep a separate parcel copy lazy until the real receiver dispatcher reads it.
        BluetoothDevice device = receive(context, roundTrip(original));
        require(device != null, "Receiver got no EXTRA_DEVICE");
        require(device != sent, "Device was not unparceled for the receiver");
        require(device.getAddress().equals("00:11:22:33:44:55"), "Unparceled device changed address");
        require(device.getAddressType() == BluetoothDevice.ADDRESS_TYPE_PUBLIC,
                "Unparceled device changed address type");
        require(source.equals(attribution.get(device)),
                "Device was not attributed to the receiving context");

        // Direct call, as Intent.prepareToEnterProcess makes it for a non-dispatcher path.
        BluetoothDevice other = BluetoothDevice.createFromHost("66:77:88:99:AA:BB",
                BluetoothDevice.ADDRESS_TYPE_RANDOM);
        require(attribution.get(other) == null, "Device attribution source should initially be null");
        other.prepareToEnterProcess(source);
        require(source.equals(attribution.get(other)),
                "prepareToEnterProcess did not set the attribution source");

        // Both overloads are present in the pinned framework. Direct calls guarantee that
        // each executes; reflection enumeration must not select and skip an overload.
        for (int overload = 0; overload < 2; overload++) {
            BluetoothDevice directDevice = BluetoothDevice.createFromHost("44:55:66:77:88:99",
                    BluetoothDevice.ADDRESS_TYPE_PUBLIC);
            BluetoothDevice scanDevice = BluetoothDevice.createFromHost("55:66:77:88:99:AA",
                    BluetoothDevice.ADDRESS_TYPE_RANDOM);
            ScanResult scan = new ScanResult(scanDevice, null, -70, 123456789L);
            scan.setAttributionSource(source);
            Intent direct = new Intent(BluetoothDevice.ACTION_FOUND);
            direct.putExtra(BluetoothDevice.EXTRA_DEVICE, directDevice);
            direct.putExtra("gesher.test.SCAN_RESULT", scan);
            Intent prepared = roundTrip(direct);
            BluetoothDevice preparedDevice = prepared.getParcelableExtra(
                    BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class);
            ScanResult preparedScan = prepared.getParcelableExtra(
                    "gesher.test.SCAN_RESULT", ScanResult.class);
            require(preparedDevice != null && preparedDevice != directDevice,
                    "Direct intent did not unmarshal EXTRA_DEVICE");
            require(preparedScan != null && preparedScan.getDevice() != null
                            && preparedScan.getDevice() != scanDevice,
                    "Direct intent did not unmarshal the scan result's device");
            require(attribution.get(preparedDevice) == null
                            && attribution.get(preparedScan.getDevice()) == null,
                    "Parcel unexpectedly retained attribution");
            if (overload == 0) {
                prepared.prepareToEnterProcess(false, source);
            } else {
                prepared.prepareToEnterProcess(0, source);
            }
            require(source.equals(attribution.get(preparedDevice)),
                    "Direct Intent overload " + overload + " did not attribute EXTRA_DEVICE");
            // AOSP's fix-up only handles EXTRA_DEVICE. An arbitrary ScanResult extra
            // stays unattributed until its real setAttributionSource entry point is called.
            require(attribution.get(preparedScan.getDevice()) == null,
                    "Intent incorrectly attributed an arbitrary ScanResult extra");
            preparedScan.setAttributionSource(source);
            require(source.equals(attribution.get(preparedScan.getDevice())),
                    "ScanResult.setAttributionSource did not attribute its device");
            require(preparedScan.getDevice().getAddressType() == BluetoothDevice.ADDRESS_TYPE_RANDOM
                            && preparedScan.getRssi() == -70
                            && preparedScan.getTimestampNanos() == 123456789L,
                    "Scan result data changed during attribution");
        }

        ScanResult scanNull = new ScanResult(null, null, -50, 0);
        scanNull.setAttributionSource(source);
        require(scanNull.getDevice() == null, "Null device in ScanResult did not remain null");

        Intent unrelated = new Intent("gesher.test.UNRELATED");
        unrelated.putExtra(BluetoothDevice.EXTRA_DEVICE, sent);
        unrelated = roundTrip(unrelated);
        unrelated.prepareToEnterProcess(false, source);
        BluetoothDevice unrelatedDevice = unrelated.getParcelableExtra(
                BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class);
        require(unrelatedDevice != null && attribution.get(unrelatedDevice) == null,
                "Non-Bluetooth intent unexpectedly attributed its device");

        System.out.println("BluetoothBroadcastAttributionTest all passed: " + checks + " checks");
    }
}
