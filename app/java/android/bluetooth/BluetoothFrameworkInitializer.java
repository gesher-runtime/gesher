/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.app.SystemServiceRegistry;
import android.content.Context;

/**
 * The Bluetooth module's initializer, which SystemServiceRegistry's static initializer
 * calls (framework.jar compiles against the module's API stubs and does not contain it).
 * It registers {@link BluetoothManager} as {@code bluetooth}, for a device without
 * Bluetooth: its adapter is null, as on a device that has none. Apps that ask for the
 * service or name the class (Waze's beacon status) failed with NoClassDefFoundError.
 */
public final class BluetoothFrameworkInitializer {
    private static volatile android.os.BluetoothServiceManager sServiceManager;
    private static volatile java.util.function.Consumer<Context> sBinderCallsStatsInitializer;

    private BluetoothFrameworkInitializer() {
    }

    /**
     * ActivityThread.main ({@code initializeMainlineModules}) hands the module the
     * process's service manager once, as the module's own initializer keeps it (ADR 0098).
     */
    public static void setBluetoothServiceManager(
            android.os.BluetoothServiceManager bluetoothServiceManager) {
        if (sServiceManager != null) {
            throw new IllegalStateException("setBluetoothServiceManager called twice!");
        }
        if (bluetoothServiceManager == null) throw new NullPointerException("bluetoothServiceManager");
        sServiceManager = bluetoothServiceManager;
    }

    public static android.os.BluetoothServiceManager getBluetoothServiceManager() {
        return sServiceManager;
    }

    /** As the module's: kept for the module's binder statistics (not collected here). */
    public static void setBinderCallsStatsInitializer(
            java.util.function.Consumer<Context> binderCallsStatsConsumer) {
        if (sBinderCallsStatsInitializer != null) {
            throw new IllegalStateException("setBinderCallsStatsInitializer called twice!");
        }
        if (binderCallsStatsConsumer == null) throw new NullPointerException("binderCallsStatsConsumer");
        sBinderCallsStatsInitializer = binderCallsStatsConsumer;
    }

    public static void registerServiceWrappers() {
        SystemServiceRegistry.registerContextAwareService(Context.BLUETOOTH_SERVICE,
                BluetoothManager.class,
                (SystemServiceRegistry.ContextAwareServiceProducerWithoutBinder<BluetoothManager>)
                        BluetoothManager::new);
    }
}
