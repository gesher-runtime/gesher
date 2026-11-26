// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
package gesher;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.os.Looper;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.FutureTask;

public final class SensorContract {
    private static native String[] probe(String mode);

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("library path and mode");
        String mode = args[1];
        boolean android = mode.equals("android");
        if (!android) System.loadLibrary("android_runtime");
        System.load(args[0]);

        // NDK queues without callbacks require a Looper which permits identifier returns.
        FutureTask<String[]> nativeRun = new FutureTask<>(() -> probe(mode));
        new Thread(nativeRun, "native-sensor-probe").start();
        String[] nativeSensors = nativeRun.get();
        for (String sensor : nativeSensors) System.out.println("NDK sensor: " + sensor);

        if (!android) {
            Looper.prepare();
            Context context = new ContextWrapper(null) {
                @Override public ApplicationInfo getApplicationInfo() {
                    ApplicationInfo info = new ApplicationInfo();
                    info.targetSdkVersion = 36;
                    return info;
                }
                @Override public String getPackageName() { return "gesher.sensors.fixture"; }
                public String getOpPackageName() { return getPackageName(); }
                @Override public int getDeviceId() { return 0; }
            };
            SensorManager manager = (SensorManager) Class.forName("android.hardware.SystemSensorManager")
                    .getConstructor(Context.class, Looper.class).newInstance(context, Looper.myLooper());
            List<String> javaSensors = new ArrayList<>();
            for (Sensor s : manager.getSensorList(Sensor.TYPE_ALL)) {
                javaSensors.add(s.getType() + "\t" + s.getName() + "\t" + s.getVendor() + "\t"
                        + s.getStringType() + "\t" + s.getMinDelay() + "\t" + s.getReportingMode() + "\t"
                        + s.getFifoMaxEventCount() + "\t" + (s.isWakeUpSensor() ? 1 : 0));
            }
            if (!javaSensors.equals(Arrays.asList(nativeSensors))) {
                throw new AssertionError("sensor metadata differs: Java=" + javaSensors
                        + " NDK=" + Arrays.toString(nativeSensors));
            }
        }
        System.out.println("SensorContract all passed (" + mode + ", " + nativeSensors.length + " sensors)");
    }
}
