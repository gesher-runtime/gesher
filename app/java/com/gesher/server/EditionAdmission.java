/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.os.ServiceSpecificException;

/** Shared reason codes for the trusted host launcher. */
public final class EditionAdmission {
    public static final int UPGRADE_REQUIRED = 6401;
    public static final int LICENSE_UNAVAILABLE = 6402;
    public static final int TRIAL_LIMIT = 6403;

    private EditionAdmission() {}

    static boolean isHostLocationFeature(String name) {
        return PackageManager.FEATURE_LOCATION.equals(name)
                || PackageManager.FEATURE_LOCATION_NETWORK.equals(name)
                || PackageManager.FEATURE_LOCATION_GPS.equals(name);
    }

    static String unavailableRequiredFeature(EditionPolicy edition, ApkPackage pkg) {
        if (edition.permits(EditionPolicy.Capability.HOST_LOCATION)) return null;
        // Evaluate individual required declarations. A permission, optional feature or
        // member of an alternative feature group does not establish a requirement here.
        for (FeatureInfo feature : pkg.parsed.getRequestedFeatures()) {
            if ((feature.flags & FeatureInfo.FLAG_REQUIRED) != 0 && isHostLocationFeature(feature.name)
                    && PackageManagerService.hasBaseSystemFeature(feature.name, feature.version)) {
                return feature.name;
            }
        }
        return null;
    }

    static final class Denied extends ServiceSpecificException {
        private final String mFeature;

        Denied(EditionPolicy edition, String packageName, String feature) {
            super(edition.upgradeAvailable() ? UPGRADE_REQUIRED
                            : edition.storeStatus() == EditionPolicy.StoreStatus.UNCONFIGURED
                                    ? TRIAL_LIMIT : LICENSE_UNAVAILABLE,
                    packageName + "\t" + feature);
            mFeature = feature;
        }

        String androidMessage() {
            return "Required feature is unavailable: " + mFeature;
        }
    }
}
