/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.hardware.usb.IDisplayPortAltModeInfoListener;
import android.hardware.usb.IUsbManager;
import android.hardware.usb.IUsbOperationInternal;
import android.hardware.usb.ParcelableUsbPort;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbOperationInternal;
import android.hardware.usb.UsbPort;
import android.hardware.usb.UsbPortStatus;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.UserHandle;

import java.util.List;
import java.util.Objects;

/**
 * Android USB service with a Windows host adapter. Accessory, gadget and USB port
 * operations use AOSP's no-manager behavior. MANAGE_USB and ACCESS_MTP remain
 * system-only. The service is also registered without a host adapter so applications
 * can obtain {@link UsbManager} and see an empty inventory (issue #303).
 */
final class UsbService extends IUsbManager.Default {
    private static final String NO_DEVICE_MANAGER = "DeviceManager must not be null";

    private final CallerIdentity mCallers;
    private final UsbHost mHost;

    UsbService(CallerIdentity callers, PackageManagerService packages, ActivityManagerService am) {
        mCallers = callers;
        mHost = new UsbHost(callers, packages, am);
    }

    void startHost() { mHost.start(); }

    private void enforce(String permission) {
        if (!mCallers.isSystem()) {
            throw new SecurityException("Requires " + permission);
        }
    }

    private void requireManageUsb() {
        enforce(android.Manifest.permission.MANAGE_USB);
    }

    /** UsbUserPermissionManager's {@code uid == SYSTEM_UID}: the system's own callers. */
    private boolean systemHasPermission() {
        return mCallers.isSystem();
    }

    /**
     * UsbUserPermissionManager.requestPermissionDialog's package check: a package that is not
     * the caller's is IllegalArgumentException ("package ... not found").
     */
    private void requireCallerPackage(String packageName) {
        try {
            mCallers.requirePackage(packageName);
        } catch (SecurityException e) {
            throw new IllegalArgumentException("package " + packageName + " not found");
        }
    }

    private static void operationError(IUsbOperationInternal callback) {
        try {
            callback.onOperationComplete(UsbOperationInternal.USB_OPERATION_ERROR_INTERNAL);
        } catch (RemoteException e) {
            System.err.println("usb: onOperationComplete failed: " + e);
        }
    }

    // Host devices and per-attachment permissions.

    @Override
    public void getDeviceList(Bundle devices) {
        mHost.list(devices);
    }

    @Override
    public ParcelFileDescriptor openDevice(String deviceName, String packageName) {
        return mHost.open(deviceName, packageName);
    }

    @Override
    public boolean hasDevicePermission(UsbDevice device, String packageName) {
        return mHost.has(device.getDeviceName(), packageName);
    }

    @Override
    public boolean hasDevicePermissionWithIdentity(UsbDevice device, String packageName,
            int pid, int uid) {
        requireManageUsb();
        return uid == android.os.Process.SYSTEM_UID && mHost.has(device.getDeviceName(), packageName);
    }

    @Override
    public void requestDevicePermission(UsbDevice device, String packageName, PendingIntent pi) {
        if (!systemHasPermission()) requireCallerPackage(packageName);
        mHost.request(device, packageName, Objects.requireNonNull(pi));
    }

    static void sendDevicePermission(PendingIntent pi, UsbDevice device, boolean allowed) {
        Intent result = new Intent();
        result.putExtra(UsbManager.EXTRA_DEVICE, device);
        result.putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, allowed);
        send(pi, result);
    }

    @Override
    public void setUsbDeviceConnectionHandler(ComponentName handler) {
        requireManageUsb();
        // No privileged fixed-handler override.
    }

    // Accessory (device mode) side: no UsbDeviceManager.

    @Override
    public UsbAccessory getCurrentAccessory() {
        return null;
    }

    @Override
    public ParcelFileDescriptor openAccessory(UsbAccessory accessory) {
        return null;
    }

    @Override
    public ParcelFileDescriptor openAccessoryForInputStream(UsbAccessory accessory) {
        return null;
    }

    @Override
    public ParcelFileDescriptor openAccessoryForOutputStream(UsbAccessory accessory) {
        return null;
    }

    @Override
    public int getMaxPacketSize(UsbAccessory accessory) {
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    @Override
    public boolean isAccessoryFfsEnabled() {
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    @Override
    public ParcelFileDescriptor getControlFd(long function) {
        enforce(android.Manifest.permission.ACCESS_MTP);
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    @Override
    public boolean hasAccessoryPermission(UsbAccessory accessory) {
        return systemHasPermission();
    }

    @Override
    public boolean hasAccessoryPermissionWithIdentity(UsbAccessory accessory, int pid, int uid) {
        requireManageUsb();
        return uid == android.os.Process.SYSTEM_UID;
    }

    @Override
    public void requestAccessoryPermission(UsbAccessory accessory, String packageName,
            PendingIntent pi) {
        Intent result = new Intent();
        result.putExtra(UsbManager.EXTRA_ACCESSORY, accessory);
        if (systemHasPermission()) {
            result.putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, true);
        } else {
            requireCallerPackage(packageName);
            result.putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
        }
        send(pi, result);
    }

    @Override
    public void setCurrentFunctions(long functions, int operationId) {
        requireManageUsb();
        if (!UsbManager.areSettableFunctions(functions)) throw new IllegalArgumentException();
        throw new IllegalStateException();  // Preconditions.checkState(mDeviceManager != null)
    }

    @Override
    public void setCurrentFunction(String function, boolean usbDataUnlocked, int operationId) {
        setCurrentFunctions(UsbManager.usbFunctionsFromString(function), operationId);
    }

    @Override
    public boolean isFunctionEnabled(String function) {
        getCurrentFunctions();  // MANAGE_USB, then no device mode
        return false;
    }

    @Override
    public boolean isUvcGadgetSupportEnabled() {
        return android.os.SystemProperties.getBoolean("ro.usb.uvc.enabled", false);
    }

    @Override
    public long getCurrentFunctions() {
        requireManageUsb();
        throw new IllegalStateException();
    }

    @Override
    public void setScreenUnlockedFunctions(long functions) {
        requireManageUsb();
        if (!UsbManager.areSettableFunctions(functions)) throw new IllegalArgumentException();
        throw new IllegalStateException();
    }

    @Override
    public long getScreenUnlockedFunctions() {
        requireManageUsb();
        throw new IllegalStateException();
    }

    @Override
    public int getCurrentUsbSpeed() {
        requireManageUsb();
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    @Override
    public int getGadgetHalVersion() {
        requireManageUsb();
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    @Override
    public void resetUsbGadget() {
        requireManageUsb();
        throw new NullPointerException(NO_DEVICE_MANAGER);
    }

    // Persistent preferences and USB activity handlers (MANAGE_USB).

    @Override
    public void setDevicePackage(UsbDevice device, String packageName, int userId) {
        Objects.requireNonNull(device);
        requireManageUsb();
    }

    @Override
    public void setAccessoryPackage(UsbAccessory accessory, String packageName, int userId) {
        Objects.requireNonNull(accessory);
        requireManageUsb();
    }

    @Override
    public void addDevicePackagesToPreferenceDenied(UsbDevice device, String[] packageNames,
            UserHandle user) {
        deniedArguments(device, packageNames, user);
    }

    @Override
    public void addAccessoryPackagesToPreferenceDenied(UsbAccessory accessory,
            String[] packageNames, UserHandle user) {
        deniedArguments(accessory, packageNames, user);
    }

    @Override
    public void removeDevicePackagesFromPreferenceDenied(UsbDevice device,
            String[] packageNames, UserHandle user) {
        deniedArguments(device, packageNames, user);
    }

    @Override
    public void removeAccessoryPackagesFromPreferenceDenied(UsbAccessory accessory,
            String[] packageNames, UserHandle user) {
        deniedArguments(accessory, packageNames, user);
    }

    private void deniedArguments(Object target, String[] packageNames, UserHandle user) {
        Objects.requireNonNull(target);
        Objects.requireNonNull(packageNames, "packageNames");
        for (String name : packageNames) {
            if (name == null) throw new NullPointerException("packageNames contains a null element");
        }
        Objects.requireNonNull(user);
        requireManageUsb();
    }

    @Override
    public void setDevicePersistentPermission(UsbDevice device, int uid, UserHandle user,
            boolean shouldBeGranted) {
        Objects.requireNonNull(device);
        Objects.requireNonNull(user);
        requireManageUsb();
    }

    @Override
    public void setAccessoryPersistentPermission(UsbAccessory accessory, int uid,
            UserHandle user, boolean shouldBeGranted) {
        Objects.requireNonNull(accessory);
        Objects.requireNonNull(user);
        requireManageUsb();
    }

    @Override
    public void grantDevicePermission(UsbDevice device, int uid) {
        requireManageUsb();
    }

    @Override
    public void grantAccessoryPermission(UsbAccessory accessory, int uid) {
        requireManageUsb();
    }

    @Override
    public boolean hasDefaults(String packageName, int userId) {
        if (packageName == null || packageName.isEmpty()) throw new IllegalArgumentException();
        requireManageUsb();
        return false;
    }

    @Override
    public void clearDefaults(String packageName, int userId) {
        if (packageName == null || packageName.isEmpty()) throw new IllegalArgumentException();
        requireManageUsb();
    }

    // Ports (MANAGE_USB): no UsbPortManager.

    @Override
    public List<ParcelableUsbPort> getPorts() {
        requireManageUsb();
        return null;  // UsbManager.getPorts() answers an empty list
    }

    @Override
    public UsbPortStatus getPortStatus(String portId) {
        Objects.requireNonNull(portId, "portId must not be null");
        requireManageUsb();
        return null;
    }

    @Override
    public boolean isModeChangeSupported(String portId) {
        requireManageUsb();
        Objects.requireNonNull(portId, "portId must not be null");
        return false;
    }

    @Override
    public void setPortRoles(String portId, int powerRole, int dataRole) {
        Objects.requireNonNull(portId, "portId must not be null");
        UsbPort.checkRoles(powerRole, dataRole);
        requireManageUsb();
    }

    @Override
    public void resetUsbPort(String portId, int operationId, IUsbOperationInternal callback) {
        Objects.requireNonNull(portId, "resetUsbPort: portId must not be null. opId:" + operationId);
        Objects.requireNonNull(callback, "resetUsbPort: callback must not be null. opId:" + operationId);
        requireManageUsb();
        operationError(callback);
    }

    @Override
    public boolean enableUsbData(String portId, boolean enable, int operationId,
            IUsbOperationInternal callback) {
        Objects.requireNonNull(portId, "enableUsbData: portId must not be null. opId:" + operationId);
        Objects.requireNonNull(callback, "enableUsbData: callback must not be null. opId:" + operationId);
        requireManageUsb();
        operationError(callback);
        return false;
    }

    @Override
    public void enableUsbDataWhileDocked(String portId, int operationId,
            IUsbOperationInternal callback) {
        Objects.requireNonNull(portId,
                "enableUsbDataWhileDocked: portId must not be null. opId:" + operationId);
        Objects.requireNonNull(callback,
                "enableUsbDataWhileDocked: callback must not be null. opId:" + operationId);
        requireManageUsb();
        operationError(callback);
    }

    @Override
    public void enableLimitPowerTransfer(String portId, boolean limit, int operationId,
            IUsbOperationInternal callback) {
        Objects.requireNonNull(portId, "portId must not be null. opID:" + operationId);
        requireManageUsb();
        operationError(callback);
    }

    @Override
    public void enableContaminantDetection(String portId, boolean enable) {
        Objects.requireNonNull(portId, "portId must not be null");
        requireManageUsb();
    }

    @Override
    public int getUsbHalVersion() {
        requireManageUsb();
        return UsbManager.USB_HAL_NOT_SUPPORTED;
    }

    @Override
    public boolean registerForDisplayPortEvents(IDisplayPortAltModeInfoListener listener) {
        Objects.requireNonNull(listener, "registerForDisplayPortEvents: listener must not be null.");
        requireManageUsb();
        return false;
    }

    @Override
    public void unregisterForDisplayPortEvents(IDisplayPortAltModeInfoListener listener) {
        Objects.requireNonNull(listener, "unregisterForDisplayPortEvents: listener must not be null.");
        requireManageUsb();
    }

    @Override
    public void enablePciTunnels(boolean enable) {
        requireManageUsb();
        // No USB4 manager: nothing to tunnel.
    }

    /** Answers a permission request through the caller's PendingIntent. */
    private static void send(PendingIntent pi, Intent fillIn) {
        if (pi == null) throw new NullPointerException("PendingIntent is null");
        try {
            pi.getTarget().send(0, fillIn, null, null, null, null, null);
        } catch (RemoteException | RuntimeException e) {
            // As UsbUserPermissionManager ignores PendingIntent.CanceledException.
            System.err.println("usb: permission result not delivered: " + e);
        }
    }
}
