/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.util.AtomicFile;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/**
 * The source-package/UID delegate preference in Android's PreferencesHelper.
 * Gesher retains UID tombstones after uninstall, so the existing installation
 * owner tokens also distinguish a new installation from the former grantee.
 * Process death and verified updates leave these installation tokens unchanged.
 */
final class NotificationDelegates {
    private static final int MAGIC = 0x474e4431; // GND1
    private static final int MAX_BYTES = 4096;
    private final File directory = new File(Device.dataDir(), "system/notification-delegates");
    private final Map<String, Grant> grants = new HashMap<>();
    private final Set<String> loaded = new HashSet<>();

    private record Grant(int sourceUid, String sourceOwner, String delegatePackage,
            int delegateUid, String delegateOwner, String delegateCode, boolean enabled) { }

    private AtomicFile file(String packageName) {
        if (packageName == null
                || !packageName.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*")) {
            throw new IllegalArgumentException("Invalid notification owner package");
        }
        return new AtomicFile(new File(directory, packageName + ".delegate"));
    }

    private static String owner(ApkPackage pkg) {
        String owner = pkg == null ? null : pkg.enabledStateOwner();
        if (owner == null) throw new IllegalStateException("Notification package has no installation owner");
        return owner;
    }

    private static String code(ApkPackage pkg) {
        // Installer rejects updates without verified signer continuity. Its
        // installation marker therefore remains authoritative across signed
        // updates, including permitted signer rotation. Unsigned developer code
        // has no such continuity and must retain the exact bytes granted access.
        return pkg.hasVerifiedSigningIdentity() ? "verified-install"
                : NotificationManagerService.codeIdentity(pkg);
    }

    private Grant load(ApkPackage source) {
        if (source == null) return null;
        String name = source.packageName;
        if (!loaded.contains(name)) {
            AtomicFile file = file(name);
            try (FileInputStream bytes = file.openRead(); DataInputStream input = new DataInputStream(bytes)) {
                if (bytes.getChannel().size() > MAX_BYTES || input.readInt() != MAGIC) {
                    throw new IOException("Invalid notification delegate record");
                }
                Grant grant = new Grant(input.readInt(), input.readUTF(), input.readUTF(),
                        input.readInt(), input.readUTF(), input.readUTF(), input.readBoolean());
                if (grant.sourceUid < 0 || grant.delegateUid < 0
                        || !UUID.fromString(grant.sourceOwner).toString().equals(grant.sourceOwner)
                        || !UUID.fromString(grant.delegateOwner).toString().equals(grant.delegateOwner)
                        || grant.delegatePackage.isEmpty() || grant.delegateCode.isEmpty()
                        || input.read() != -1) {
                    throw new IOException("Invalid notification delegate identity");
                }
                grants.put(name, grant);
            } catch (java.io.FileNotFoundException absent) {
                if (file.getBaseFile().exists()) {
                    throw new IllegalStateException("Cannot read notification delegate for " + name, absent);
                }
            } catch (IOException | IllegalArgumentException failure) {
                throw new IllegalStateException("Cannot read notification delegate for " + name, failure);
            }
            loaded.add(name);
        }
        Grant grant = grants.get(name);
        return grant != null && grant.sourceUid == source.uid()
                && grant.sourceOwner.equals(owner(source)) ? grant : null;
    }

    synchronized String get(ApkPackage source) {
        Grant grant = load(source);
        // Android keeps the configured name after the delegate is uninstalled.
        // Authorization separately compares the currently installed identity.
        return grant != null && grant.enabled ? grant.delegatePackage : null;
    }

    synchronized boolean allows(ApkPackage source, ApkPackage delegate) {
        Grant grant = load(source);
        return grant != null && grant.enabled && delegate != null
                && grant.delegatePackage.equals(delegate.packageName)
                && grant.delegateUid == delegate.uid()
                && grant.delegateOwner.equals(owner(delegate))
                && grant.delegateCode.equals(code(delegate));
    }

    synchronized void set(ApkPackage source, ApkPackage delegate) {
        Grant before = load(source);
        if (delegate == null && before == null) return;
        Grant next = delegate == null
                ? new Grant(before.sourceUid, before.sourceOwner, before.delegatePackage,
                        before.delegateUid, before.delegateOwner, before.delegateCode, false)
                : new Grant(source.uid(), owner(source), delegate.packageName,
                        delegate.uid(), owner(delegate), code(delegate), true);
        if (next.delegateCode == null) {
            throw new IllegalStateException("Cannot identify notification delegate code");
        }
        if (next.equals(before)) return;
        AtomicFile file = file(source.packageName);
        FileOutputStream bytes = null;
        try {
            bytes = file.startWrite();
            DataOutputStream output = new DataOutputStream(bytes);
            output.writeInt(MAGIC);
            output.writeInt(next.sourceUid); output.writeUTF(next.sourceOwner);
            output.writeUTF(next.delegatePackage); output.writeInt(next.delegateUid);
            output.writeUTF(next.delegateOwner); output.writeUTF(next.delegateCode);
            output.writeBoolean(next.enabled);
            output.flush(); bytes.getFD().sync();
            file.finishWrite(bytes);
            if (new File(file.getBaseFile().getPath() + ".new").exists()) {
                throw new IOException("Atomic notification delegate commit failed");
            }
            bytes = null;
        } catch (IOException | RuntimeException failure) {
            if (bytes != null) file.failWrite(bytes);
            throw new IllegalStateException("Cannot save notification delegate for " + source.packageName, failure);
        }
        grants.put(source.packageName, next);
        loaded.add(source.packageName);
    }

    synchronized void remove(String packageName) {
        AtomicFile file = file(packageName);
        file.delete();
        for (String suffix : new String[]{"", ".new", ".bak"}) {
            if (new File(file.getBaseFile().getPath() + suffix).exists()) {
                throw new IllegalStateException("Cannot remove notification delegate for " + packageName);
            }
        }
        grants.remove(packageName);
        loaded.remove(packageName);
    }
}
