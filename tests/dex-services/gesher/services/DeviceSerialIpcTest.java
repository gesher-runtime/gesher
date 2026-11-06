/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.ServiceManager;

/** Standard framework API over real Binder; no hardware identifier is queried or printed. */
public final class DeviceSerialIpcTest {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        IBinder remote = ServiceManager.getService("device_identifiers");
        if (remote == null || remote instanceof Binder) throw new AssertionError("Device identifiers service is not remote");
        String serial = Build.getSerial();
        if (args.length == 1 && args[0].equals("expect-null")) {
            if (serial != null) throw new AssertionError("Local-only Binder negative control did not return null");
            System.out.println("DeviceSerialIpcTest: expected null negative control passed");
        } else {
            if (!Build.UNKNOWN.equals(serial)) throw new AssertionError("Framework serial API did not return Build.UNKNOWN");
            System.out.println("DeviceSerialIpcTest: remote Build.getSerial equals Build.UNKNOWN");
        }
    }
}
