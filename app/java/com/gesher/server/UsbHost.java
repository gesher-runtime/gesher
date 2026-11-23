/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.hardware.usb.IUsbSerialReader;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.IntFunction;

/** USB attachment inventory and attachment-scoped WinUSB application permission. */
final class UsbHost {
    private final CallerIdentity callers;
    private final PackageManagerService packages;
    private final ActivityManagerService activityManager;
    private final UsbAttachActivities attachActivities;
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "usb-host");
        thread.setDaemon(true);
        return thread;
    });
    private final Executor consent = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "usb-consent");
        thread.setDaemon(true);
        return thread;
    });
    private final Map<String, Attachment> byPath = new HashMap<>();
    private final Map<String, Attachment> byName = new HashMap<>();
    private final Set<PermissionRequest> pending = new HashSet<>();
    private int queuedAttachChoices;
    private volatile boolean choosingAttachment;
    private boolean started;
    private int nextAddress;

    UsbHost(CallerIdentity callers, PackageManagerService packages, ActivityManagerService activityManager) {
        this.callers = callers;
        this.packages = packages;
        this.activityManager = activityManager;
        attachActivities = new UsbAttachActivities(packages, IntentChooser.HOST);
    }

    synchronized void start() {
        if (started || !available()) return;
        started = true;
        DeviceWork.add(() -> choosingAttachment ? 0 : Long.MAX_VALUE);
        worker.scheduleWithFixedDelay(this::monitor, 0, 1, TimeUnit.SECONDS);
    }

    private enum Backend { WINUSB, STORAGE_OBSERVATION }

    private static final class Attachment {
        final String path;
        final long version;
        final UsbDevice device;
        final Backend backend;
        final Set<ApkPackage> grants = new HashSet<>();
        final Map<ApkPackage, ComponentName> launching = new HashMap<>();
        volatile boolean present = true;

        Attachment(String path, long version, UsbDevice device, Backend backend) {
            this.path = path;
            this.version = version;
            this.device = device;
            this.backend = backend;
        }

        boolean supportsTransfers() {
            return backend == Backend.WINUSB;
        }

        boolean hasGrant(ApkPackage installation) {
            if (grants.contains(installation)) return true;
            ComponentName target = launching.get(installation);
            return target != null && installation.componentEnabled(
                    target.getClassName(), ApkPackage.ComponentKind.ACTIVITY);
        }
    }

    private record PermissionRequest(Attachment attachment, ApkPackage installation) {}
    private record StorageDescriptors(byte[] raw, String[] strings) {}
    private record Alternate(int id, int setting, int nameIndex, int type, int subtype, int protocol,
            List<UsbEndpoint> endpoints) {}

    private static native boolean nativeAvailable();
    private static native String[] nativePaths();
    private static native String[] nativeStoragePaths();
    private static native StorageDescriptors nativeInspectStorage(String path);
    private static native long nativeVersion(String path);
    private static native byte[] nativeInspect(String path);
    private static native String nativeString(String path, int index);
    private static native int nativeOpen(String path, long version);

    static boolean available() {
        try {
            return nativeAvailable();
        } catch (LinkageError unavailable) {
            return false;
        }
    }

    synchronized void list(Bundle result) {
        refresh();
        for (Attachment device : byName.values()) result.putParcelable(device.device.getDeviceName(), device.device);
    }

    private void monitor() {
        synchronized (this) {
            try {
                refresh();
            } catch (RuntimeException failed) {
                SystemServer.log("USB inventory refresh failed: " + failed);
            }
        }
    }

    private void refresh() {
        if (!started) return;
        try {
            refresh(nativePaths(), Backend.WINUSB);
            refresh(nativeStoragePaths(), Backend.STORAGE_OBSERVATION);
        } catch (LinkageError unavailable) {
            // The no-host service stays available when the native backend is absent.
        }
    }

    private void refresh(String[] paths, Backend backend) {
        if (paths == null) return;
        Set<String> present = new HashSet<>(List.of(paths));
        var iterator = byPath.entrySet().iterator();
        while (iterator.hasNext()) {
            Attachment device = iterator.next().getValue();
            if (device.backend != backend) continue;
            if (present.contains(device.path) && nativeVersion(device.path) == device.version) continue;
            device.present = false;
            iterator.remove();
            byName.remove(device.device.getDeviceName());
            activityManager.sendBroadcastFor("android", new Intent(UsbManager.ACTION_USB_DEVICE_DETACHED)
                    .addFlags(Intent.FLAG_RECEIVER_INCLUDE_BACKGROUND)
                    .putExtra(UsbManager.EXTRA_DEVICE, device.device), null);
        }
        for (String path : paths) {
            if (byPath.containsKey(path) || nextAddress >= 999 * 127) continue;
            long version = nativeVersion(path);
            if (version < 0) continue;
            int address = nextAddress;
            String name = String.format(Locale.ROOT, "/dev/bus/usb/%03d/%03d",
                    address / 127 + 1, address % 127 + 1);
            UsbDevice device;
            if (backend == Backend.WINUSB) {
                byte[] descriptors = nativeInspect(path);
                device = descriptors == null ? null : parse(name, descriptors,
                        index -> nativeString(path, index));
            } else {
                StorageDescriptors descriptors = nativeInspectStorage(path);
                device = descriptors == null ? null : parse(name, descriptors.raw,
                        index -> descriptors.strings[index]);
            }
            if (device == null || nativeVersion(path) != version) continue;
            ++nextAddress;
            Attachment attachment = new Attachment(path, version, device, backend);
            byName.put(name, attachment);
            byPath.put(path, attachment);
            activityManager.sendBroadcastFor("android", new Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                    .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY)
                    .putExtra(UsbManager.EXTRA_DEVICE, device), null);
            if (queuedAttachChoices >= 32) continue;
            queuedAttachChoices++;
            consent.execute(() -> chooseActivity(attachment));
        }
    }

    private void chooseActivity(Attachment attachment) {
        choosingAttachment = true;
        try {
            IntentChooser.Option chosen = attachActivities.choose(attachment.device,
                    () -> stillAttached(attachment), attachment.supportsTransfers());
            if (chosen == null || !chosen.current(packages, null)) return;
            synchronized (this) {
                if (!launchTargetCurrent(attachment, chosen)) return;
                if (attachment.supportsTransfers()) {
                    attachment.launching.put(chosen.code, chosen.component);
                }
            }
            boolean launched = false;
            try {
                // The activity can open the real device from onCreate. The lifetime callback
                // runs during publication without acquiring the USB inventory or PM lock.
                int result = activityManager.startActivityFor("android", chosen.intent, null,
                        () -> launchTargetCurrent(attachment, chosen));
                launched = ActivityManager.isStartResultSuccessful(result);
                if (!launched) {
                    SystemServer.log("USB activity start failed for " + chosen.component + ": " + result);
                }
            } finally {
                synchronized (this) {
                    attachment.launching.remove(chosen.code);
                    if (launched && attachment.supportsTransfers() && launchTargetCurrent(attachment, chosen)) {
                        attachment.grants.add(chosen.code);
                    }
                }
            }
        } catch (RuntimeException | LinkageError failure) {
            SystemServer.log("USB activity selection failed: " + failure.getClass().getSimpleName());
        } finally {
            choosingAttachment = false;
            synchronized (this) {
                queuedAttachChoices--;
            }
        }
    }

    private boolean stillAttached(Attachment attachment) {
        return attachment.present && nativeVersion(attachment.path) == attachment.version;
    }

    private boolean launchTargetCurrent(Attachment attachment, IntentChooser.Option chosen) {
        return stillAttached(attachment) && installed(chosen.code)
                && chosen.code.componentEnabled(chosen.component.getClassName(), ApkPackage.ComponentKind.ACTIVITY);
    }

    private static int uint8(byte[] data, int offset) { return Byte.toUnsignedInt(data[offset]); }

    private static int uint16(byte[] data, int offset) {
        return uint8(data, offset) | (uint8(data, offset + 1) << 8);
    }

    private UsbDevice parse(String name, byte[] raw, IntFunction<String> strings) {
        if (raw.length < 27 || raw[0] != 18 || raw[1] != 1 || raw[17] != 1
                || raw[18] != 9 || raw[19] != 2 || raw[22] != 1
                || uint16(raw, 20) != raw.length - 18) return null;
        // USB media has additional Android routing and permission contracts.
        // Windows keeps ownership of hubs and boot HID input.
        if (raw[4] == 1 || raw[4] == 0x0e || raw[4] == 9) return null;
        List<Alternate> alternates = new ArrayList<>();
        Alternate current = null;
        int endpointCount = 0;
        Set<Integer> settings = new HashSet<>();
        for (int offset = 27; offset < raw.length; ) {
            int length = uint8(raw, offset);
            if (length < 2 || length > raw.length - offset) return null;
            int type = uint8(raw, offset + 1);
            if (type == 4) {
                if (length < 9 || (current != null && current.endpoints.size() != endpointCount)) return null;
                int interfaceId = uint8(raw, offset + 2);
                int alternate = uint8(raw, offset + 3);
                int ifaceClass = uint8(raw, offset + 5);
                int ifaceSubclass = uint8(raw, offset + 6);
                if (ifaceClass == 1 || ifaceClass == 0x0e || ifaceClass == 9
                        || (ifaceClass == 3 && ifaceSubclass == 1)
                        || !settings.add(alternate)
                        || (!alternates.isEmpty() && interfaceId != alternates.get(0).id)) return null;
                current = new Alternate(interfaceId, alternate, uint8(raw, offset + 8),
                        ifaceClass, ifaceSubclass,
                        uint8(raw, offset + 7), new ArrayList<>());
                alternates.add(current);
                endpointCount = uint8(raw, offset + 4);
            } else if (type == 5) {
                if (length < 7 || current == null) return null;
                int address = uint8(raw, offset + 2);
                int attributes = uint8(raw, offset + 3);
                // WinUSB synchronous transfer in Gesher handles bulk and interrupt pipes.
                if ((address & 0xf) == 0 || ((attributes & 3) != 2 && (attributes & 3) != 3)
                        || current.endpoints.stream().anyMatch(e -> e.getAddress() == address)) return null;
                current.endpoints.add(new UsbEndpoint(address, attributes, uint16(raw, offset + 4),
                        uint8(raw, offset + 6)));
            }
            offset += length;
        }
        if (current == null || current.endpoints.size() != endpointCount || !settings.contains(0)) return null;
        UsbInterface[] interfaces = new UsbInterface[alternates.size()];
        for (int i = 0; i < interfaces.length; ++i) {
            Alternate a = alternates.get(i);
            UsbInterface iface = new UsbInterface(a.id, a.setting, strings.apply(a.nameIndex),
                    a.type, a.subtype, a.protocol);
            iface.setEndpoints(a.endpoints.toArray(UsbEndpoint[]::new));
            interfaces[i] = iface;
        }
        UsbConfiguration config = new UsbConfiguration(uint8(raw, 23), strings.apply(uint8(raw, 24)),
                uint8(raw, 25), uint8(raw, 26));
        config.setInterfaces(interfaces);
        int version = uint16(raw, 12);
        return new UsbDevice.Builder(name, uint16(raw, 8), uint16(raw, 10), uint8(raw, 4),
                uint8(raw, 5), uint8(raw, 6), strings.apply(uint8(raw, 14)),
                strings.apply(uint8(raw, 15)),
                String.format(Locale.ROOT, "%x.%02x", version >>> 8, version & 255),
                new UsbConfiguration[]{config}, null, false, false, false, false, false)
                .build(serialReader(name, strings.apply(uint8(raw, 16))));
    }

    private IUsbSerialReader serialReader(String name, String serial) {
        return new IUsbSerialReader.Stub() {
            @Override public String getSerial(String packageName) {
                if (!has(name, packageName)) throw new SecurityException("No USB permission");
                return serial;
            }
        };
    }

    private Attachment valid(String name) {
        Attachment device = byName.get(name);
        return device != null && stillAttached(device) ? device : null;
    }

    private boolean installed(ApkPackage installation) {
        return installation != null && packages.get(installation.packageName) == installation
                && !packages.isRemovalPending(installation.packageName);
    }

    synchronized boolean has(String name, String packageName) {
        Attachment device = valid(name);
        if (device != null && !device.supportsTransfers()) return false;
        if (callers.isSystem()) return true;
        if (device == null) return false;
        ApkPackage installation = callers.requireCurrentPackage(packageName);
        return installed(installation) && device.hasGrant(installation);
    }

    synchronized ParcelFileDescriptor open(String name, String packageName) {
        Attachment device = valid(name);
        if (device == null || !device.supportsTransfers() || !has(name, packageName)) return null;
        int fd = nativeOpen(device.path, device.version);
        if (fd < 0) return null;
        ParcelFileDescriptor handle = ParcelFileDescriptor.adoptFd(fd);
        if (valid(name) != device) {
            try { handle.close(); } catch (java.io.IOException ignored) {}
            return null;
        }
        return handle;
    }

    synchronized void request(UsbDevice requested, String packageName, PendingIntent reply) {
        Attachment device = valid(requested.getDeviceName());
        if (device != null && !device.supportsTransfers()) {
            UsbService.sendDevicePermission(reply, device.device, false);
            return;
        }
        if (callers.isSystem()) {
            UsbService.sendDevicePermission(reply, requested, true);
            return;
        }
        ApkPackage installation = callers.requireCurrentPackage(packageName);
        if (device == null || !installed(installation)
                || !installation.packageName.equals(reply.getCreatorPackage())) {
            UsbService.sendDevicePermission(reply, requested, false);
            return;
        }
        if (device.hasGrant(installation)) {
            // A successful explicit permission reply must survive cleanup of a
            // simultaneous chooser launch that fails to publish an activity.
            device.grants.add(installation);
            UsbService.sendDevicePermission(reply, device.device, true);
            return;
        }
        PermissionRequest request = new PermissionRequest(device, installation);
        if (pending.size() >= 32 || !pending.add(request)) {
            UsbService.sendDevicePermission(reply, device.device, false);
            return;
        }
        consent.execute(() -> {
            boolean connected;
            synchronized (UsbHost.this) {
                connected = installed(installation) && valid(device.device.getDeviceName()) == device;
            }
            boolean allowed = connected && PermissionRequests.confirm(installation.packageName,
                    String.format(Locale.ROOT, "Access USB device %04x:%04x",
                            device.device.getVendorId(), device.device.getProductId()));
            synchronized (UsbHost.this) {
                pending.remove(request);
                allowed = allowed && installed(installation)
                        && valid(device.device.getDeviceName()) == device;
                if (allowed) device.grants.add(installation);
            }
            UsbService.sendDevicePermission(reply, device.device, allowed);
        });
    }
}
