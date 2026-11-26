/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Actual Client/Binder/Parcel protocol; package publication is the explicit fixture boundary. */
public final class HostInstallIntentTest {
    private static final String[] RESULT = {"fixture.host.install", "unchanged", "fixture-code", "1"};
    private static int checks;

    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError(reason);
        checks++;
    }

    private interface Call { void run() throws Exception; }

    private static void rejected(Class<? extends Throwable> type, Call call) throws Exception {
        try {
            call.run();
        } catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError("Expected " + type.getName(), failure);
            checks++;
            return;
        }
        throw new AssertionError("Expected " + type.getName());
    }

    private static class LegacyDevice implements GesherService.Device {
        final List<String> paths = new ArrayList<>();
        int touches;
        @Override public String[] install(String apk) { paths.add(apk); return RESULT; }
        @Override public boolean uninstall(String packageName) { throw new AssertionError("Unexpected uninstall"); }
        @Override public String status() { return "fixture"; }
        @Override public void shutdown(String reason) { throw new AssertionError("Unexpected shutdown"); }
        @Override public int lastExitCode(String packageName) { return Integer.MIN_VALUE; }
        @Override public void touch() { touches++; }
        @Override public void toastActivated(String appId, String arguments) { throw new AssertionError("Unexpected toast"); }
    }

    private static final class RecordingDevice extends LegacyDevice {
        final List<Boolean> intents = new ArrayList<>();
        @Override public String[] install(String apk, boolean explicitInstall) {
            intents.add(explicitInstall);
            return super.install(apk);
        }
    }

    private static GesherService.Client client(IBinder binder) throws Exception {
        Constructor<GesherService.Client> constructor = GesherService.Client.class.getDeclaredConstructor(IBinder.class);
        constructor.setAccessible(true);
        return constructor.newInstance(binder);
    }

    private static void request(GesherService service, String descriptor, int... extension) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(descriptor);
            data.writeString("fixture.apk");
            for (int value : extension) data.writeInt(value);
            check(service.transact(GesherService.INSTALL, data, reply, 0), "INSTALL transaction disappeared");
            reply.readException();
            check(Arrays.equals(reply.createStringArray(), RESULT), "Install reply changed its existing result fields");
            if (extension.length == 0) {
                check(reply.dataAvail() == 0, "Legacy install reply gained an ambiguous extension");
            } else {
                check(reply.dataAvail() == Integer.BYTES && reply.readInt() == extension[0],
                        "Extended install did not acknowledge its exact intent");
            }
        } finally {
            data.recycle(); reply.recycle();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        check(Services.sLast == null, "Run the local Binder fixture in its own process");
        RecordingDevice device = new RecordingDevice();
        GesherService service = new GesherService(device);
        GesherService.Client client = client(service);

        check(Arrays.equals(client.install("legacy.apk"), RESULT), "Legacy client result changed");
        check(Arrays.equals(client.install("launch.apk", false), RESULT), "Ordinary launch result changed");
        check(Arrays.equals(client.install("explicit.apk", true), RESULT), "Explicit install result changed");
        check(device.paths.equals(List.of("legacy.apk", "launch.apk", "explicit.apk")), "APK identity changed across Parcel");
        check(device.intents.equals(List.of(false, false, true)), "Explicit intent was lost or inferred for ordinary launch");
        request(service, GesherService.DESCRIPTOR);
        request(service, GesherService.DESCRIPTOR, 0);
        check(device.intents.equals(List.of(false, false, true, false, false)), "Missing or false intent became explicit");

        int calls = device.paths.size(), touches = device.touches;
        rejected(SecurityException.class, () -> request(service, "wrong.descriptor", 1));
        check(device.paths.size() == calls && device.touches == touches, "Wrong descriptor reached the device");
        rejected(IllegalArgumentException.class, () -> request(service, GesherService.DESCRIPTOR, -1));
        rejected(IllegalArgumentException.class, () -> request(service, GesherService.DESCRIPTOR, 2));
        rejected(IllegalArgumentException.class, () -> request(service, GesherService.DESCRIPTOR, 1, 0));
        check(device.paths.size() == calls, "Malformed intent reached package publication");

        // Older servers ignore trailing request data and return only the original array.
        // The explicit client must not report that as a completed eligibility refresh.
        int[] remaining = {-1};
        Binder oldServer = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code != GesherService.INSTALL) return false;
                data.enforceInterface(GesherService.DESCRIPTOR);
                data.readString();
                remaining[0] = data.dataAvail();
                reply.writeNoException(); reply.writeStringArray(RESULT);
                return true;
            }
        };
        GesherService.Client old = client(oldServer);
        check(Arrays.equals(old.install("legacy.apk"), RESULT) && remaining[0] == 0, "Legacy wire format changed");
        check(Arrays.equals(old.install("launch.apk", false), RESULT) && remaining[0] == 0, "Ordinary launch requires a new server");
        rejected(IllegalStateException.class, () -> old.install("explicit.apk", true));
        check(remaining[0] == Integer.BYTES, "Explicit client omitted its intent");

        LegacyDevice legacyDevice = new LegacyDevice();
        rejected(UnsupportedOperationException.class,
                () -> client(new GesherService(legacyDevice)).install("explicit.apk", true));
        check(legacyDevice.paths.isEmpty(), "Unsupported device silently downgraded explicit installation");
        System.out.println("HostInstallIntentTest " + checks + " checks passed");
    }
}
