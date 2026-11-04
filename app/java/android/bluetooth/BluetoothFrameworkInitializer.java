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
    private BluetoothFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
        SystemServiceRegistry.registerContextAwareService(Context.BLUETOOTH_SERVICE,
                BluetoothManager.class,
                (SystemServiceRegistry.ContextAwareServiceProducerWithoutBinder<BluetoothManager>)
                        BluetoothManager::new);
    }
}
