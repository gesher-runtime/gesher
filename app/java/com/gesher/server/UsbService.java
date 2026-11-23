/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.hardware.usb.IUsbManager;
import android.hardware.usb.ParcelableUsbPort;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbPortStatus;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.UserHandle;

import java.util.Collections;
import java.util.List;

/**
 * {@code usb}: UsbService of a device with no USB host stack and no accessory function,
 * as AOSP's is when the device lacks {@code FEATURE_USB_HOST} (no UsbHostManager) and
 * {@code /dev/usb_accessory} (no UsbDeviceManager). PackageManagerService declares
 * neither {@code android.hardware.usb.host} nor {@code android.hardware.usb.accessory},
 * and Windows' USB devices are not exposed to apps, so:
 * {@code UsbManager.getDeviceList()} is empty, {@code getAccessoryList()} null,
 * {@code openDevice}/{@code openAccessory} answer null, no device or accessory has
 * permission, and a permission request is answered at once with
 * {@code EXTRA_PERMISSION_GRANTED} false, the answer a phone's dialog gives when the
 * permission is not granted (there is nothing attached to grant it for). No gadget
 * functions are enabled and there are no ports; the MANAGE_USB calls stay system-only.
 *
 * <p>Without the service {@code getSystemService(USB_SERVICE)} answered null, which no
 * phone does (Baseus 2.18.0 casts it unchecked in MainActivity.onResume, issue #303).
 */
final class UsbService extends IUsbManager.Default {
    private final CallerIdentity mCallers;

    UsbService(CallerIdentity callers) {
        mCallers = callers;
    }

    private void requireManageUsb() {
        if (!mCallers.isSystem()) {
            throw new SecurityException("Requires android.permission.MANAGE_USB");
        }
    }

    // Host side: no UsbHostManager.

    @Override
    public void getDeviceList(Bundle devices) {
        // No devices attached: the bundle stays empty.
    }

    @Override
    public ParcelFileDescriptor openDevice(String deviceName, String packageName) {
        mCallers.requirePackage(packageName);
        return null;
    }

    @Override
    public boolean hasDevicePermission(UsbDevice device, String packageName) {
        mCallers.requirePackage(packageName);
        return false;
    }

    @Override
    public void requestDevicePermission(UsbDevice device, String packageName, PendingIntent pi) {
        mCallers.requirePackage(packageName);
        Intent result = new Intent();
        result.putExtra(UsbManager.EXTRA_DEVICE, device);
        result.putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
        send(pi, result);
    }

    // Accessory side: no UsbDeviceManager.

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
    public boolean hasAccessoryPermission(UsbAccessory accessory) {
        return false;
    }

    @Override
    public void requestAccessoryPermission(UsbAccessory accessory, String packageName,
            PendingIntent pi) {
        mCallers.requirePackage(packageName);
        Intent result = new Intent();
        result.putExtra(UsbManager.EXTRA_ACCESSORY, accessory);
        result.putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
        send(pi, result);
    }

    @Override
    public boolean isFunctionEnabled(String function) {
        return false;
    }

    // MANAGE_USB (system API): nothing attached, no gadget functions, no ports.

    @Override
    public boolean hasDevicePermissionWithIdentity(UsbDevice device, String packageName,
            int pid, int uid) {
        requireManageUsb();
        return false;
    }

    @Override
    public boolean hasAccessoryPermissionWithIdentity(UsbAccessory accessory, int pid, int uid) {
        requireManageUsb();
        return false;
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
    public void setDevicePackage(UsbDevice device, String packageName, int userId) {
        requireManageUsb();
    }

    @Override
    public void setAccessoryPackage(UsbAccessory accessory, String packageName, int userId) {
        requireManageUsb();
    }

    @Override
    public void setDevicePersistentPermission(UsbDevice device, int uid, UserHandle user,
            boolean shouldBeGranted) {
        requireManageUsb();
    }

    @Override
    public void setAccessoryPersistentPermission(UsbAccessory accessory, int uid,
            UserHandle user, boolean shouldBeGranted) {
        requireManageUsb();
    }

    @Override
    public boolean hasDefaults(String packageName, int userId) {
        requireManageUsb();
        return false;
    }

    @Override
    public void clearDefaults(String packageName, int userId) {
        requireManageUsb();
    }

    @Override
    public void setCurrentFunctions(long functions, int operationId) {
        requireManageUsb();
        // As UsbService's checkState(mDeviceManager != null): no device (gadget) mode.
        throw new IllegalStateException("USB device mode not supported");
    }

    @Override
    public void setCurrentFunction(String function, boolean usbDataUnlocked, int operationId) {
        requireManageUsb();
        throw new IllegalStateException("USB device mode not supported");
    }

    @Override
    public long getCurrentFunctions() {
        requireManageUsb();
        return UsbManager.FUNCTION_NONE;
    }

    @Override
    public void setScreenUnlockedFunctions(long functions) {
        requireManageUsb();
        throw new IllegalStateException("USB device mode not supported");
    }

    @Override
    public long getScreenUnlockedFunctions() {
        requireManageUsb();
        return UsbManager.FUNCTION_NONE;
    }

    @Override
    public int getCurrentUsbSpeed() {
        requireManageUsb();
        return UsbManager.USB_DATA_TRANSFER_RATE_UNKNOWN;
    }

    @Override
    public int getGadgetHalVersion() {
        requireManageUsb();
        return UsbManager.GADGET_HAL_NOT_SUPPORTED;
    }

    @Override
    public int getUsbHalVersion() {
        requireManageUsb();
        return UsbManager.USB_HAL_NOT_SUPPORTED;
    }

    @Override
    public List<ParcelableUsbPort> getPorts() {
        requireManageUsb();
        return Collections.emptyList();
    }

    @Override
    public UsbPortStatus getPortStatus(String portId) {
        requireManageUsb();
        return null;
    }

    @Override
    public boolean isModeChangeSupported(String portId) {
        requireManageUsb();
        return false;
    }

    @Override
    public void setUsbDeviceConnectionHandler(ComponentName handler) {
        requireManageUsb();
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
