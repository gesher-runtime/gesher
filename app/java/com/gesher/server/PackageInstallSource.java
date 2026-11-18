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
import java.util.Objects;
import java.util.StringJoiner;
import java.util.UUID;

/** Immutable installation history and the independently mutable installer of record. */
record PackageInstallSource(String initiator, String initiatingOwner, String installer,
        String installationOwner, int category, SigningDetails signingDetails) {
    PackageInstallSource {
        // UNKNOWN is not a certificate snapshot and cannot construct a SigningInfo.
        if (signingDetails == SigningDetails.UNKNOWN) signingDetails = null;
    }

    /** The original registry format used one identity for both roles. */
    PackageInstallSource(String installer, String installationOwner, int category,
            SigningDetails signingDetails) {
        this(installer, installationOwner, installer, installationOwner, category, signingDetails);
    }

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

    /** AOSP InstallSource.setInstallerPackage preserves the original request and signer. */
    PackageInstallSource withInstaller(ApkPackage candidate) {
        String name = candidate == null ? null : candidate.packageName;
        String owner = candidate == null ? null : candidate.enabledStateOwner();
        if (Objects.equals(installer, name) && Objects.equals(installationOwner, owner)) return this;
        return new PackageInstallSource(initiator, initiatingOwner, name, owner, category, signingDetails);
    }

    /** Owners survive an update, but a committed uninstall retires both relationships. */
    boolean isInstaller(ApkPackage candidate) {
        return owns(candidate, installer, installationOwner);
    }

    boolean isInitiator(ApkPackage candidate) {
        return owns(candidate, initiator, initiatingOwner);
    }

    private static boolean owns(ApkPackage candidate, String name, String owner) {
        return candidate != null && name != null && name.equals(candidate.packageName)
                && owner != null && owner.equals(candidate.enabledStateOwner());
    }

    SigningInfo signingInfo() {
        return signingDetails == null ? null : new SigningInfo(signingDetails);
    }

    /** One optional tab-separated registry column; certificates contain hexadecimal bytes. */
    String encode() {
        String signing = signingDetails == null ? "" : "," + signingDetails.getSignatureSchemeVersion()
                + "," + encode(signingDetails.getSignatures(), false)
                + "," + encode(signingDetails.getPastSigningCertificates(), true);
        if (Objects.equals(initiator, installer) && Objects.equals(initiatingOwner, installationOwner)) {
            if (installer == null) {
                return category == PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED ? "" : ",," + category;
            }
            return installer + "," + installationOwner + "," + category + signing;
        }
        // v2 separates history from current attribution. Empty signer fields are explicit.
        return "v2," + text(initiator) + "," + text(initiatingOwner) + "," + text(installer)
                + "," + text(installationOwner) + "," + category
                + (signing.isEmpty() ? ",,," : signing);
    }

    private static String text(String value) { return value == null ? "" : value; }
    private static String optional(String value) { return value.isEmpty() ? null : value; }

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
        if (fields.length == 9 && fields[0].equals("v2")) {
            String initiator = optional(fields[1]), initiatingOwner = optional(fields[2]);
            String installer = optional(fields[3]), installerOwner = optional(fields[4]);
            checkIdentity(initiator, initiatingOwner);
            checkIdentity(installer, installerOwner);
            int category = checkedCategory(fields[5]);
            SigningDetails details = readSigning(fields[6], fields[7], fields[8]);
            if (initiator == null && details != null) throw new IllegalArgumentException("Signer without initiator");
            return new PackageInstallSource(initiator, initiatingOwner, installer, installerOwner,
                    category, details);
        }
        if (fields.length != 3 && fields.length != 6) {
            throw new IllegalArgumentException("Invalid installer attribution");
        }
        int category = checkedCategory(fields[2]);
        if (fields.length == 3 && fields[0].isEmpty() && fields[1].isEmpty()) {
            return withoutInstaller(category);
        }
        checkIdentity(fields[0], fields[1]);
        return new PackageInstallSource(fields[0], fields[1], category, fields.length == 6
                ? readSigning(fields[3], fields[4], fields[5]) : null);
    }

    private static int checkedCategory(String value) {
        int category = Integer.parseInt(value);
        if (category(category) != category) throw new IllegalArgumentException("Invalid package source");
        return category;
    }

    static void checkIdentity(String name, String owner) {
        if (name == null && owner == null) return;
        if (name == null || owner == null
                || !name.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*")
                || !UUID.fromString(owner).toString().equals(owner)) {
            throw new IllegalArgumentException("Invalid installer attribution");
        }
    }

    private static SigningDetails readSigning(String version, String signers, String past)
            throws CertificateException {
        if (version.isEmpty() && signers.isEmpty() && past.isEmpty()) return null;
        int scheme = Integer.parseInt(version);
        Signature[] current = decodeSignatures(signers, false);
        Signature[] history = decodeSignatures(past, true);
        if (scheme < SigningDetails.SignatureSchemeVersion.JAR
                || scheme > SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3
                || current == null || (history != null && (current.length != 1
                        || !current[0].equals(history[history.length - 1])))) {
            throw new IllegalArgumentException("Invalid installer signing details");
        }
        return new SigningDetails(current, scheme, history);
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
