/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import com.gesher.server.WindowsBluetooth;
import java.lang.reflect.Field;

/** Production cache access, without loading a native backend on the host JVM. */
public final class BluetoothSnapshotTest {
    public static void main(String[] args) throws Exception {
        if (WindowsBluetooth.cachedAdapter(10_000) != null) throw new AssertionError("empty cache");
        WindowsBluetooth.Adapter observed = WindowsBluetooth.adapter();
        if (observed.hr != 0x80004001) throw new AssertionError("host unexpectedly has JNI");
        if (WindowsBluetooth.cachedAdapter(10_000) != observed) throw new AssertionError("recent snapshot");
        Field time = WindowsBluetooth.class.getDeclaredField("sAdapterAt");
        time.setAccessible(true);
        time.setLong(null, System.nanoTime() / 1_000_000 - 20_000);
        if (WindowsBluetooth.cachedAdapter(10_000) != null) throw new AssertionError("stale snapshot");
        time.setLong(null, System.nanoTime() / 1_000_000 + 20_000);
        if (WindowsBluetooth.cachedAdapter(10_000) != null) throw new AssertionError("future snapshot");
        WindowsBluetooth.invalidate();
        if (WindowsBluetooth.cachedAdapter(10_000) != null) throw new AssertionError("invalidated snapshot");
        System.out.println("PASS: actual Bluetooth observation cache freshness and invalidation");
    }
}
