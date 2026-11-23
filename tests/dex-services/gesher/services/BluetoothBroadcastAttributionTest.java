/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.bluetooth.BluetoothDevice;
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
 * Issue #303: a registered receiver of a Bluetooth broadcast carrying EXTRA_DEVICE. The
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

    public static void main(String[] args) throws Exception {
        Context context = systemContext("bluetooth-broadcast-test");
        require(BluetoothDevice.class.getClassLoader() == Object.class.getClassLoader(),
                "BluetoothDevice is not the boot class path's (gesher-app.jar) class");

        BluetoothDevice sent = BluetoothDevice.createFromHost("00:11:22:33:44:55",
                BluetoothDevice.ADDRESS_TYPE_PUBLIC);
        Intent original = new Intent(BluetoothDevice.ACTION_FOUND);
        original.putExtra(BluetoothDevice.EXTRA_DEVICE, sent);
        Parcel parcel = Parcel.obtain();
        Intent delivered;
        try {
            original.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            delivered = Intent.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }

        HandlerThread thread = new HandlerThread("bluetooth-receiver");
        thread.start();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<BluetoothDevice> received = new AtomicReference<>();
        CountDownLatch done = new CountDownLatch(1);
        thread.setUncaughtExceptionHandler((t, e) -> {
            failure.set(e);
            done.countDown();
        });
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) {
                received.set(intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class));
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
        BluetoothDevice device = received.get();
        require(device != null, "Receiver got no EXTRA_DEVICE");
        require(device != sent, "Device was not unparceled for the receiver");
        require(device.getAddress().equals("00:11:22:33:44:55"), "Unparceled device changed address");
        require(device.getAddressType() == BluetoothDevice.ADDRESS_TYPE_PUBLIC,
                "Unparceled device changed address type");
        Field attribution = BluetoothDevice.class.getDeclaredField("mAttributionSource");
        attribution.setAccessible(true);
        AttributionSource source = (AttributionSource) attribution.get(device);
        require(source != null && source.equals(context.getAttributionSource()),
                "Device was not attributed to the receiving context");

        // Direct call, as Intent.prepareToEnterProcess makes it for a non-dispatcher path.
        BluetoothDevice other = BluetoothDevice.createFromHost("66:77:88:99:AA:BB",
                BluetoothDevice.ADDRESS_TYPE_RANDOM);
        other.prepareToEnterProcess(context.getAttributionSource());
        require(context.getAttributionSource().equals(attribution.get(other)),
                "prepareToEnterProcess did not set the attribution source");
        thread.quitSafely();
        System.out.println("BluetoothBroadcastAttributionTest all passed: " + checks + " checks");
    }
}
