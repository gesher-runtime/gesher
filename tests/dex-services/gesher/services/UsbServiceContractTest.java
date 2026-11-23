/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.PendingIntent;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.IBinder;
import java.util.ArrayList;
import java.util.List;

/**
 * Issue #303: getSystemService(USB_SERVICE) is the real framework UsbManager over Gesher's
 * {@code usb} service, describing a device with no USB host stack and no accessory
 * function: no devices, no accessories, no permission, and a permission request answered
 * with a denial, consistent with the absent USB features.
 */
public final class UsbServiceContractTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    /** Records what the service sends through the caller's PendingIntent. */
    private static final class Sender extends IIntentSender.Stub {
        final List<Intent> sent = new ArrayList<>();
        @Override public void send(int code, Intent intent, String resolvedType, IBinder allowlistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            sent.add(intent);
        }
    }

    public static void main(String[] args) throws Exception {
        Context context = BluetoothBroadcastAttributionTest.systemContext("usb-test");

        Object named = context.getSystemService(Context.USB_SERVICE);
        require(named instanceof UsbManager, "getSystemService(USB_SERVICE) is not a UsbManager");
        UsbManager usb = context.getSystemService(UsbManager.class);
        require(usb == named, "Typed UsbManager differs from the named one");
        require(UsbManager.class.getClassLoader() == Object.class.getClassLoader(),
                "UsbManager is not the production framework class");

        require(usb.getDeviceList() != null && usb.getDeviceList().isEmpty(),
                "Device list is not empty: no USB device is exposed");
        require(usb.getAccessoryList() == null, "An accessory was reported");

        // The package service's declared features (what ApplicationPackageManager caches
        // in an app process; this bare fixture has no SystemFeaturesCache).
        android.content.pm.IPackageManager pm = android.app.ActivityThread.getPackageManager();
        require(!pm.hasSystemFeature(PackageManager.FEATURE_USB_HOST, 0),
                "USB host feature declared without a host stack");
        require(!pm.hasSystemFeature(PackageManager.FEATURE_USB_ACCESSORY, 0),
                "USB accessory feature declared without an accessory function");

        // A device the app names (for example from a stale intent): not attached here.
        UsbDevice device = new UsbDevice.Builder("/dev/bus/usb/001/002", 0x1234, 0x5678, 0, 0, 0,
                "maker", "product", "1.0", new UsbConfiguration[0], null,
                false, false, false, false, false).build(new android.hardware.usb.IUsbSerialReader.Stub() {
                    @Override public String getSerial(String packageName) { return null; }
                });
        require(usb.openDevice(device) == null, "A device that is not attached was opened");
        require(!usb.hasPermission(device), "Permission reported for a device that is not attached");

        Sender sender = new Sender();
        usb.requestPermission(device, new PendingIntent(sender));
        require(sender.sent.size() == 1, "Permission request was not answered exactly once");
        Intent answer = sender.sent.get(0);
        require(!answer.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, true),
                "Permission request was not denied");
        UsbDevice answered = answer.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        require(answered != null && answered.getDeviceName().equals(device.getDeviceName()),
                "Denial did not name the requested device");

        // System API (MANAGE_USB): this fixture is the system server's own process.
        require(usb.getPorts().isEmpty(), "A USB port was reported");
        require(usb.getCurrentFunctions() == UsbManager.FUNCTION_NONE, "A gadget function is enabled");
        boolean refused = false;
        try {
            usb.setCurrentFunctions(UsbManager.FUNCTION_MTP);
        } catch (IllegalStateException expected) {
            refused = true;
        }
        require(refused, "Gadget function change reported success without device mode");
        System.out.println("UsbServiceContractTest all passed: " + checks + " checks");
    }
}
