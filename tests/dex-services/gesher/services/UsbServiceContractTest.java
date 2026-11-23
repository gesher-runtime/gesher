/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.PendingIntent;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.hardware.usb.IDisplayPortAltModeInfoListener;
import android.hardware.usb.IUsbManager;
import android.hardware.usb.IUsbOperationInternal;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbOperationInternal;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ServiceManager;
import java.util.ArrayList;
import java.util.List;

/**
 * Issue #303: getSystemService(USB_SERVICE) is the real framework UsbManager over Gesher's
 * {@code usb} service, which answers as AOSP's UsbService does with no host, device or port
 * manager (an emulator without USB): no devices or accessories, no ports, port operations
 * reporting an internal error, device-mode calls failing, permission the system's only, and
 * MANAGE_USB/ACCESS_MTP calls refused to applications. The application half runs under an
 * application's Binder identity (an unattached app UID and PID).
 */
public final class UsbServiceContractTest {
    private static final int APP_UID = 10123;
    private static final int APP_PID = 999_999;
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private interface Call { void run() throws Exception; }

    private static void throwsType(Class<? extends Throwable> type, Call call, String reason)
            throws Exception {
        try {
            call.run();
        } catch (Throwable t) {
            require(type.isInstance(t), reason + ": got " + t);
            return;
        }
        throw new AssertionError(reason + ": no exception");
    }

    /** Records what the service sends through the caller's PendingIntent. */
    private static final class Sender extends IIntentSender.Stub {
        final List<Intent> sent = new ArrayList<>();
        @Override public void send(int code, Intent intent, String resolvedType, IBinder allowlistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            sent.add(intent);
        }
    }

    private static final class Operation extends IUsbOperationInternal.Stub {
        final List<Integer> results = new ArrayList<>();
        @Override public void onOperationComplete(int status) { results.add(status); }
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

        // A device named by the caller (for example from a stale intent): not attached here.
        UsbDevice device = new UsbDevice.Builder("/dev/bus/usb/001/002", 0x1234, 0x5678, 0, 0, 0,
                "maker", "product", "1.0", new UsbConfiguration[0], null,
                false, false, false, false, false).build(new android.hardware.usb.IUsbSerialReader.Stub() {
                    @Override public String getSerial(String packageName) { return null; }
                });
        require(usb.openDevice(device) == null, "A device that is not attached was opened");

        // System half: this fixture is the system server's own process (AOSP's
        // uid == SYSTEM_UID rule grants it permission, without a dialog).
        require(usb.hasPermission(device), "The system was denied device permission");
        Sender systemSender = new Sender();
        usb.requestPermission(device, new PendingIntent(systemSender));
        require(systemSender.sent.size() == 1, "System permission request not answered once");
        require(systemSender.sent.get(0).getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false),
                "System permission request was not granted");
        require(usb.getPorts().isEmpty(), "A USB port was reported");
        throwsType(IllegalStateException.class, usb::getCurrentFunctions,
                "Gadget functions read without device mode");
        throwsType(IllegalStateException.class, () -> usb.setCurrentFunctions(UsbManager.FUNCTION_MTP),
                "Gadget function change without device mode");
        throwsType(IllegalStateException.class, () -> usb.isFunctionEnabled(UsbManager.USB_FUNCTION_MTP),
                "Gadget function query without device mode");

        IUsbManager service = IUsbManager.Stub.asInterface(ServiceManager.getService(Context.USB_SERVICE));
        require(service != null, "No usb Binder service");
        Operation reset = new Operation();
        service.resetUsbPort("port0", 7, reset);
        require(reset.results.size() == 1
                && reset.results.get(0) == UsbOperationInternal.USB_OPERATION_ERROR_INTERNAL,
                "Port reset did not report an internal error");
        Operation data = new Operation();
        require(!service.enableUsbData("port0", true, 8, data), "enableUsbData reported waiting");
        require(data.results.size() == 1
                && data.results.get(0) == UsbOperationInternal.USB_OPERATION_ERROR_INTERNAL,
                "enableUsbData did not report an internal error");
        throwsType(NullPointerException.class, service::getGadgetHalVersion,
                "Gadget HAL version answered without a device manager");
        require(service.getUsbHalVersion() == UsbManager.USB_HAL_NOT_SUPPORTED,
                "A USB HAL was reported");

        // Application half: an application's Binder identity, as a remote app call has.
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) APP_UID << 32) | APP_PID);
            require(Binder.getCallingUid() == APP_UID, "Application identity not in effect");
            Bundle devices = new Bundle();
            service.getDeviceList(devices);
            require(devices.isEmpty(), "Application saw a device");
            require(service.openDevice(device.getDeviceName(), "com.example.other") == null,
                    "Application opened a device");
            require(!service.hasDevicePermission(device, "com.example.other"),
                    "Application has device permission");
            require(!service.hasAccessoryPermission(null), "Application has accessory permission");
            require(service.getCurrentAccessory() == null, "Application saw an accessory");
            Sender forged = new Sender();
            throwsType(IllegalArgumentException.class,
                    () -> service.requestDevicePermission(device, "com.example.forged",
                            new PendingIntent(forged)),
                    "Permission request for another package was accepted");
            require(forged.sent.isEmpty(), "A forged permission request was answered");

            Operation refused = new Operation();
            Call[] manageUsb = {
                    service::getCurrentFunctions,
                    () -> service.setCurrentFunctions(UsbManager.FUNCTION_NONE, 1),
                    () -> service.isFunctionEnabled(UsbManager.USB_FUNCTION_MTP),
                    service::getScreenUnlockedFunctions,
                    service::getCurrentUsbSpeed,
                    service::getGadgetHalVersion,
                    service::resetUsbGadget,
                    service::getPorts,
                    () -> service.getPortStatus("port0"),
                    () -> service.isModeChangeSupported("port0"),
                    () -> service.setPortRoles("port0", 0, 0),
                    () -> service.resetUsbPort("port0", 2, refused),
                    () -> service.enableUsbData("port0", false, 3, refused),
                    () -> service.enableUsbDataWhileDocked("port0", 4, refused),
                    () -> service.enableLimitPowerTransfer("port0", true, 5, refused),
                    () -> service.enableContaminantDetection("port0", true),
                    service::getUsbHalVersion,
                    () -> service.registerForDisplayPortEvents(new IDisplayPortAltModeInfoListener.Default()),
                    () -> service.unregisterForDisplayPortEvents(new IDisplayPortAltModeInfoListener.Default()),
                    () -> service.enablePciTunnels(true),
                    () -> service.grantDevicePermission(device, APP_UID),
                    () -> service.hasDevicePermissionWithIdentity(device, "com.example.other", 1, 1000),
                    () -> service.setDevicePackage(device, "com.example.other", 0),
                    () -> service.addDevicePackagesToPreferenceDenied(device,
                            new String[] {"com.example.other"}, android.os.UserHandle.SYSTEM),
                    () -> service.removeDevicePackagesFromPreferenceDenied(device,
                            new String[] {"com.example.other"}, android.os.UserHandle.SYSTEM),
                    () -> service.setDevicePersistentPermission(device, APP_UID,
                            android.os.UserHandle.SYSTEM, true),
                    () -> service.hasDefaults("com.example.other", 0),
                    () -> service.clearDefaults("com.example.other", 0),
                    () -> service.setUsbDeviceConnectionHandler(null),
                    () -> service.getControlFd(UsbManager.FUNCTION_MTP),
            };
            for (int i = 0; i < manageUsb.length; i++) {
                throwsType(SecurityException.class, manageUsb[i],
                        "Privileged USB call " + i + " was allowed to an application");
            }
            require(refused.results.isEmpty(), "A refused port operation reported a result");
        } finally {
            Binder.restoreCallingIdentity(original);
        }
        System.out.println("UsbServiceContractTest all passed: " + checks + " checks");
    }
}
