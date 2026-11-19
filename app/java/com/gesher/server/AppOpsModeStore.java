/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.pm.Signature;
import android.util.AtomicFile;
import android.util.Xml;
import com.android.modules.utils.TypedXmlPullParser;
import com.android.modules.utils.TypedXmlSerializer;
import org.xmlpull.v1.XmlPullParser;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Durable mode snapshots. Reads never wait for serialization or fsync. */
final class AppOpsModeStore {
    record Owner(int uid, String name, String signer) {
        boolean matches(ApkPackage pkg) {
            return pkg != null && pkg.uid() == uid && name.equals(pkg.packageName)
                    && Objects.equals(signer, AppOpsModeStore.signer(pkg));
        }
    }
    record UidOp(int uid, int op) { }
    record PackageOp(int uid, String name, int op) { }
    record UidMode(int mode, List<Owner> owners) {
        UidMode { owners = List.copyOf(owners); }
    }
    record PackageMode(int mode, Owner owner) { }
    record State(Map<UidOp, UidMode> uids, Map<PackageOp, PackageMode> packages) {
        State { uids = Map.copyOf(uids); packages = Map.copyOf(packages); }
        static State empty() { return new State(Map.of(), Map.of()); }
    }

    private final AtomicFile mFile;
    private volatile State mState;

    AppOpsModeStore(File file) {
        mFile = file == null ? null : new AtomicFile(file);
        mState = read();
    }
    State state() { return mState; }
    void reload() { if (mFile != null) mState = read(); }

    private static final Map<ApkPackage, String> SIGNERS = new java.util.WeakHashMap<>();
    static String signer(ApkPackage pkg) {
        synchronized (SIGNERS) {
            if (SIGNERS.containsKey(pkg)) return SIGNERS.get(pkg);
        }
        String signer = computeSigner(pkg);
        synchronized (SIGNERS) { SIGNERS.put(pkg, signer); }
        return signer;
    }
    private static String computeSigner(ApkPackage pkg) {
        Signature[] signatures = pkg.verifiedSignatures();
        if (signatures == null || signatures.length == 0) return null;
        try {
            List<String> certificates = new ArrayList<>();
            for (Signature signature : signatures) {
                byte[] digest = MessageDigest.getInstance("SHA-256").digest(signature.toByteArray());
                StringBuilder hex = new StringBuilder();
                for (byte value : digest) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                certificates.add(hex.toString());
            }
            Collections.sort(certificates);
            return String.join(":", certificates);
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    Owner owner(ApkPackage pkg) {
        String signer = signer(pkg);
        if (mFile != null && signer == null) {
            throw new IllegalStateException("Cannot persist AppOps without verified signing identity");
        }
        return new Owner(pkg.uid(), pkg.packageName, signer);
    }

    /** Caller serializes writers; publication follows a successful atomic file commit. */
    void commit(State next) {
        if (mFile != null) {
            FileOutputStream stream = null;
            try {
                stream = mFile.startWrite();
                TypedXmlSerializer xml = Xml.resolveSerializer(stream);
                xml.startDocument(null, true);
                xml.startTag(null, "app-ops");
                xml.attributeInt(null, "version", 2);
                for (var entry : next.uids.entrySet()) {
                    xml.startTag(null, "uid-op");
                    xml.attributeInt(null, "uid", entry.getKey().uid);
                    xml.attributeInt(null, "op", entry.getKey().op);
                    xml.attributeInt(null, "mode", entry.getValue().mode);
                    for (Owner owner : entry.getValue().owners) writeOwner(xml, owner);
                    xml.endTag(null, "uid-op");
                }
                for (var entry : next.packages.entrySet()) {
                    xml.startTag(null, "package-op");
                    xml.attributeInt(null, "op", entry.getKey().op);
                    xml.attributeInt(null, "mode", entry.getValue().mode);
                    writeOwner(xml, entry.getValue().owner);
                    xml.endTag(null, "package-op");
                }
                xml.endTag(null, "app-ops");
                xml.endDocument();
                mFile.finishWrite(stream);
            } catch (Exception failure) {
                if (stream != null) mFile.failWrite(stream);
                throw new IllegalStateException("Cannot commit AppOps modes", failure);
            }
        }
        mState = next;
    }

    private static void writeOwner(TypedXmlSerializer xml, Owner owner) throws IOException {
        xml.startTag(null, "owner");
        xml.attributeInt(null, "uid", owner.uid);
        xml.attribute(null, "package", owner.name);
        xml.attribute(null, "signer", Objects.requireNonNull(owner.signer));
        xml.endTag(null, "owner");
    }

    private State read() {
        if (mFile == null) return State.empty();
        try (FileInputStream stream = mFile.openRead()) {
            TypedXmlPullParser xml = Xml.resolvePullParser(stream);
            while (xml.next() != XmlPullParser.START_TAG) {
                if (xml.getEventType() == XmlPullParser.END_DOCUMENT) throw new IOException("Empty AppOps state");
            }
            if (!"app-ops".equals(xml.getName()) || xml.getAttributeInt(null, "version", -1) != 2) {
                // The previous experimental file had no authenticated owners. It cannot confer
                // grants to an installed or subsequently reused UID. Normal defaults still apply.
                SystemServer.log("AppOps: ignoring legacy modes without signing identity");
                return State.empty();
            }
            Map<UidOp, UidMode> uids = new HashMap<>();
            Map<PackageOp, PackageMode> packages = new HashMap<>();
            int root = xml.getDepth();
            while (xml.next() != XmlPullParser.END_DOCUMENT && xml.getDepth() >= root) {
                if (xml.getEventType() != XmlPullParser.START_TAG) continue;
                String element = xml.getName();
                if (!"uid-op".equals(element) && !"package-op".equals(element)) {
                    throw new IOException("Unknown AppOps record");
                }
                int op = xml.getAttributeInt(null, "op");
                int mode = xml.getAttributeInt(null, "mode");
                AppOpsService.validateOp(op); AppOpsService.validateMode(mode);
                if (AppOpsManager.opToSwitch(op) != op) throw new IOException("Non-switch AppOps record");
                int uid = xml.getAttributeInt(null, "uid", -1);
                int depth = xml.getDepth();
                List<Owner> owners = new ArrayList<>();
                while (xml.next() != XmlPullParser.END_DOCUMENT) {
                    if (xml.getEventType() == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
                    if (xml.getEventType() != XmlPullParser.START_TAG) continue;
                    if (!"owner".equals(xml.getName())) throw new IOException("Unknown AppOps owner");
                    int ownerUid = xml.getAttributeInt(null, "uid");
                    String name = xml.getAttributeValue(null, "package");
                    String signer = xml.getAttributeValue(null, "signer");
                    if (ownerUid < 0 || name == null || name.isEmpty() || signer == null
                            || !signer.matches("[0-9a-f]{64}(:[0-9a-f]{64})*")) {
                        throw new IOException("Invalid AppOps owner");
                    }
                    owners.add(new Owner(ownerUid, name, signer));
                }
                if (owners.isEmpty()) throw new IOException("Missing AppOps owner");
                if ("uid-op".equals(element)) {
                    if (uid < 0) throw new IOException("Invalid AppOps UID");
                    for (Owner owner : owners) if (owner.uid != uid) throw new IOException("Owner UID mismatch");
                    if (uids.put(new UidOp(uid, op), new UidMode(mode, owners)) != null) {
                        throw new IOException("Duplicate UID mode");
                    }
                } else {
                    if (owners.size() != 1) throw new IOException("Invalid package owners");
                    Owner owner = owners.get(0);
                    if (packages.put(new PackageOp(owner.uid, owner.name, op), new PackageMode(mode, owner)) != null) {
                        throw new IOException("Duplicate package mode");
                    }
                }
            }
            return new State(uids, packages);
        } catch (FileNotFoundException absent) {
            return State.empty();
        } catch (Exception failure) {
            // Never expose a partially parsed policy as successful state.
            throw new IllegalStateException("Cannot read AppOps modes", failure);
        }
    }
}
