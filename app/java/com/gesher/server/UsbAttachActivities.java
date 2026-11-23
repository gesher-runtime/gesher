/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.hardware.usb.DeviceFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.util.DisplayMetrics;

import com.android.internal.util.XmlUtils;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.function.BooleanSupplier;

/** Resolve a hardware attachment against the installed activities' Android USB filters. */
final class UsbAttachActivities {
    private final PackageManagerService packages;
    private final IntentChooser.Dialog dialog;

    UsbAttachActivities(PackageManagerService packages, IntentChooser.Dialog dialog) {
        this.packages = packages;
        this.dialog = dialog;
    }

    IntentChooser.Option choose(UsbDevice device, BooleanSupplier attached, boolean supportsTransfers) {
        if (!attached.getAsBoolean() || !dialog.available()) return null;
        Intent intent = new Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra(UsbManager.EXTRA_DEVICE, device);
        List<IntentChooser.Option> options = resolve(device, intent);
        if (options.isEmpty() || !attached.getAsBoolean()) return null;
        String[] labels = new String[options.size()];
        boolean[] enabled = new boolean[options.size()];
        for (int i = 0; i < labels.length; i++) {
            labels[i] = options.get(i).label;
            enabled[i] = true;
        }
        String title = String.format(Locale.ROOT, supportsTransfers
                        ? "Choose an app to access USB device %04x:%04x"
                        : "Choose an app for attached USB device %04x:%04x",
                device.getVendorId(), device.getProductId());
        int index = dialog.choose("Android USB", title, labels, enabled, attached);
        if (index < 0 || index >= options.size() || !attached.getAsBoolean()) return null;
        IntentChooser.Option chosen = options.get(index);
        return chosen.current(packages, null) ? chosen : null;
    }

    private record Candidate(ResolveInfo resolved, ApkPackage installation) {}

    private List<IntentChooser.Option> resolve(UsbDevice device, Intent intent) {
        List<Candidate> candidates = new ArrayList<>();
        // Tie the resolved ActivityInfo and its resources to the same installed APK generation.
        synchronized (packages) {
            @SuppressWarnings("unchecked")
            List<ResolveInfo> resolvedActivities = packages.queryIntentActivities(
                    intent, null, PackageManager.GET_META_DATA, 0).getList();
            for (ResolveInfo resolved : resolvedActivities) {
                ActivityInfo info = resolved.activityInfo;
                if (!IntentChooser.eligible(packages, null, info) || info.metaData == null
                        || (info.applicationInfo.flags & ApplicationInfo.FLAG_SUSPENDED) != 0) continue;
                ApkPackage installation = packages.get(info.packageName);
                if (installation != null && !packages.isRemovalPending(info.packageName)) {
                    candidates.add(new Candidate(resolved, installation));
                }
            }
        }
        int priority = Integer.MIN_VALUE;
        Set<ComponentName> components = new HashSet<>();
        List<IntentChooser.Option> options = new ArrayList<>();
        for (Candidate candidate : candidates) {
            ResolveInfo resolved = candidate.resolved;
            // Android caps positive activity-filter priorities for unprivileged APKs.
            int effectivePriority = Math.min(0, resolved.priority);
            int resource = resolved.activityInfo.metaData.getInt(UsbManager.ACTION_USB_DEVICE_ATTACHED);
            if (effectivePriority < priority || resource == 0
                    || !matches(resolved.activityInfo.applicationInfo, resource, device)) continue;
            IntentChooser.Option option = new IntentChooser.Option(new Intent(intent), resolved,
                    candidate.installation, null);
            if (!option.current(packages, null)) continue;
            if (effectivePriority > priority) {
                priority = effectivePriority;
                options.clear();
                components.clear();
            }
            if (components.add(option.component)) options.add(option);
        }
        return options.size() <= IntentChooser.MAX_OPTIONS ? options : List.of();
    }

    private static boolean matches(ApplicationInfo app, int resource, UsbDevice device) {
        try (AssetManager assets = new AssetManager()) {
            if (assets.addAssetPath(app.sourceDir) == 0) return false;
            if (app.splitSourceDirs != null) {
                for (String split : app.splitSourceDirs) {
                    if (assets.addAssetPath(split) == 0) return false;
                }
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            Configuration config = new Configuration();
            config.setToDefaults();
            Resources resources = new Resources(assets, metrics, config);
            try (XmlResourceParser xml = resources.getXml(resource)) {
                XmlUtils.nextElement(xml);
                while (xml.getName() != null) {
                    if ("usb-device".equals(xml.getName()) && DeviceFilter.read(xml).matches(device)) {
                        return true;
                    }
                    XmlUtils.nextElement(xml);
                }
            }
        } catch (Exception failure) {
            SystemServer.log("USB activity has invalid device metadata in " + app.packageName
                    + " (" + failure.getClass().getSimpleName() + ")");
        }
        return false;
    }
}
