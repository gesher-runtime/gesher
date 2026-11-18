/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageInstaller;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;

import java.security.cert.CertificateException;
import java.util.StringJoiner;
import java.util.UUID;

/** Attribution for an authenticated session, committed with the target's code registry entry. */
record PackageInstallSource(String installer, String installationOwner, int category,
        SigningDetails signingDetails) {
    static final PackageInstallSource UNKNOWN = new PackageInstallSource(null, null,
            PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED, null);

    static PackageInstallSource fromSession(ApkPackage owner, int requestedCategory) {
        int category = category(requestedCategory);
        if (owner == null) return withoutInstaller(category);
        return new PackageInstallSource(owner.packageName, owner.enabledStateOwner(),
                category, owner.verifiedSigningDetails());
    }

    private static PackageInstallSource withoutInstaller(int category) {
        return category == PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED
                ? UNKNOWN : new PackageInstallSource(null, null, category, null);
    }

    static int category(int category) {
        return switch (category) {
            case PackageInstaller.PACKAGE_SOURCE_OTHER, PackageInstaller.PACKAGE_SOURCE_STORE,
                    PackageInstaller.PACKAGE_SOURCE_LOCAL_FILE,
                    PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE -> category;
            default -> PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED;
        };
    }

    /** The owner's marker is stable across updates and retired with a committed uninstall. */
    boolean isInstaller(ApkPackage candidate) {
        return candidate != null && installer != null && installer.equals(candidate.packageName)
                && installationOwner != null && installationOwner.equals(candidate.enabledStateOwner());
    }

    SigningInfo signingInfo() {
        return signingDetails == null ? null : new SigningInfo(signingDetails);
    }

    /** One optional tab-separated registry column; certificates contain only hexadecimal bytes. */
    String encode() {
        if (installer == null) {
            return category == PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED ? "" : ",," + category;
        }
        String identity = installer + "," + installationOwner + "," + category;
        if (signingDetails == null) return identity;
        return identity + "," + signingDetails.getSignatureSchemeVersion()
                + "," + encode(signingDetails.getSignatures(), false)
                + "," + encode(signingDetails.getPastSigningCertificates(), true);
    }

    private static String encode(Signature[] signatures, boolean withFlags) {
        StringJoiner result = new StringJoiner(";");
        if (signatures != null) {
            for (Signature signature : signatures) {
                String entry = signature.toCharsString();
                result.add(withFlags ? entry + ":" + signature.getFlags() : entry);
            }
        }
        return result.toString();
    }

    static PackageInstallSource decode(String value) throws CertificateException {
        if (value.isEmpty()) return UNKNOWN;
        String[] fields = value.split(",", -1);
        if (fields.length != 3 && fields.length != 6) {
            throw new IllegalArgumentException("Invalid installer attribution");
        }
        int category = Integer.parseInt(fields[2]);
        if (category(category) != category) throw new IllegalArgumentException("Invalid package source");
        if (fields.length == 3 && fields[0].isEmpty() && fields[1].isEmpty()) {
            return withoutInstaller(category);
        }
        if (!fields[0].matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*")
                || !UUID.fromString(fields[1]).toString().equals(fields[1])) {
            throw new IllegalArgumentException("Invalid installer attribution");
        }
        SigningDetails details = null;
        if (fields.length == 6) {
            int scheme = Integer.parseInt(fields[3]);
            Signature[] current = decodeSignatures(fields[4], false);
            Signature[] history = decodeSignatures(fields[5], true);
            if (scheme < SigningDetails.SignatureSchemeVersion.JAR
                    || scheme > SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3
                    || current == null || (history != null && (current.length != 1
                            || !current[0].equals(history[history.length - 1])))) {
                throw new IllegalArgumentException("Invalid installer signing details");
            }
            details = new SigningDetails(current, scheme, history);
        }
        return new PackageInstallSource(fields[0], fields[1], category, details);
    }

    private static Signature[] decodeSignatures(String encoded, boolean withFlags) {
        if (encoded.isEmpty()) return null;
        String[] parts = encoded.split(";", -1);
        Signature[] signatures = new Signature[parts.length];
        for (int i = 0; i < parts.length; i++) {
            int separator = parts[i].indexOf(':');
            if (parts[i].isEmpty() || withFlags != (separator >= 0)) {
                throw new IllegalArgumentException("Invalid installer signer");
            }
            signatures[i] = new Signature(withFlags ? parts[i].substring(0, separator) : parts[i]);
            if (withFlags) signatures[i].setFlags(Integer.parseInt(parts[i].substring(separator + 1)));
        }
        return signatures;
    }
}
