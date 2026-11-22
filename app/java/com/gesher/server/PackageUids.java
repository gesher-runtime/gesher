/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.locks.ReentrantLock;

/** Stable admitted Android app identities. This is Binder identity, not a Windows sandbox. */
public final class PackageUids {
    private PackageUids() { }
    private static final ReentrantLock REGISTRY_LOCK = new ReentrantLock();
    private static final String NEXT = "next-app-id";

    private static String group(String value) { return value == null || value.isEmpty() ? null : value; }
    private static void name(String value) {
        if (value == null || value.isEmpty() || value.indexOf(':') >= 0
                || "android".equals(value) || "com.android.shell".equals(value)) {
            throw new IllegalArgumentException("Invalid application identity");
        }
    }

    /** Allocate an isolated identity for server-owned tools; this cannot join a shared UID. */
    public static int uidFor(String packageName) {
        name(packageName);
        try (Admission admission = new Admission()) {
            String existing = admission.after.getProperty(packageName);
            if (existing != null) return Integer.parseInt(existing);
            int uid = allocate(admission.after);
            admission.after.setProperty(packageName, Integer.toString(uid));
            admission.commit(); admission.accept();
            return uid;
        }
    }

    /** A shared identity can only be looked up after authenticated package admission. */
    public static int uidFor(String packageName, String sharedUserId) {
        if (group(sharedUserId) == null) return uidFor(packageName);
        name(packageName);
        try (Admission admission = new Admission()) {
            if (!sharedUserId.equals(admission.after.getProperty("shared-pkg:" + packageName))
                    || !"true".equals(admission.after.getProperty("active:" + packageName))
                    || readSigner(admission.after, packageName) == SigningDetails.UNKNOWN) {
                throw new SecurityException("Shared UID requires authenticated package admission");
            }
            return Integer.parseInt(admission.after.getProperty(packageName));
        }
    }

    /** The shared identity survives a member's removal, until the final member retires. */
    static SigningDetails sharedSigningDetails(ApkPackage member) {
        String shared = group(member.sharedUserId());
        if (shared == null) return member.verifiedSigningDetails();
        try (Admission admission = new Admission()) {
            Properties ids = admission.after;
            if (!shared.equals(ids.getProperty("shared-pkg:" + member.packageName))
                    || !"true".equals(ids.getProperty("active:" + member.packageName))
                    || !Integer.toString(member.uid()).equals(ids.getProperty(member.packageName))) {
                throw new SecurityException("Shared UID requires a current admitted member");
            }
            SigningDetails signing = sharedSigner(ids, shared);
            if (signing == SigningDetails.UNKNOWN) throw new SecurityException("Shared UID has no verified signer");
            return signing;
        }
    }

    private static SigningDetails sharedSigner(Properties ids, String shared) {
        SigningDetails signing = readSigner(ids, "shared:" + shared);
        if (signing != SigningDetails.UNKNOWN) return signing;
        // Older registries have only per-package records. Merge all their verified
        // active members; the next admission/retirement persists the shared snapshot.
        for (String key : ids.stringPropertyNames()) {
            if (!key.startsWith("shared-pkg:") || !shared.equals(ids.getProperty(key))) continue;
            String member = key.substring("shared-pkg:".length());
            if (!"true".equals(ids.getProperty("active:" + member))) continue;
            SigningDetails peer = readSigner(ids, member);
            if (peer == SigningDetails.UNKNOWN) throw new SecurityException("Shared UID has no verified owner");
            signing = signing == SigningDetails.UNKNOWN ? peer : signing.mergeLineageWith(peer,
                    SigningDetails.CapabilityMergeRule.MERGE_RESTRICTED_CAPABILITY);
        }
        return signing;
    }

    /** Validate the complete candidate before boot, direct launch, or install publication. */
    static Admission prepare(Collection<ApkPackage> incoming, Collection<ApkPackage> installed) {
        List<ApkPackage> pending = new ArrayList<>();
        for (ApkPackage pkg : incoming) if (pkg.uidPending()) pending.add(pkg);
        if (pending.isEmpty()) return new Admission(false);
        Map<String, ApkPackage> current = new HashMap<>();
        for (ApkPackage pkg : installed) current.put(pkg.packageName, pkg);
        Map<String, SigningDetails> verified = new LinkedHashMap<>();
        for (ApkPackage pkg : pending) {
            name(pkg.packageName);
            SigningDetails signing = pkg.verifiedSigningDetails();
            String shared = group(pkg.sharedUserId());
            if (shared != null && (shared.startsWith("android.uid.") || signing == SigningDetails.UNKNOWN)) {
                throw new SecurityException("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE: " + pkg.packageName);
            }
            ApkPackage previous = current.get(pkg.packageName);
            if (previous != null && !Objects.equals(shared, group(previous.sharedUserId()))) {
                throw new SecurityException("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE: " + pkg.packageName);
            }
            if (previous != null && previous.hasVerifiedSigningIdentity() && !pkg.updateContinuityFrom(previous)) {
                throw new SecurityException("INSTALL_FAILED_UPDATE_INCOMPATIBLE: " + pkg.packageName);
            }
            if (verified.put(pkg.packageName, signing) != null) {
                throw new IllegalArgumentException("Duplicate package in identity admission");
            }
        }
        Admission admission = new Admission();
        try {
            Properties ids = admission.after;
            // Populate private candidate records first, so validation is independent
            // of the order in which a boot scan happens to enumerate packages.
            for (ApkPackage pkg : pending) {
                String pkgName = pkg.packageName;
                String shared = group(pkg.sharedUserId());
                boolean active = "true".equals(ids.getProperty("active:" + pkgName));
                String oldGroup = group(ids.getProperty("shared-pkg:" + pkgName));
                SigningDetails old = readSigner(ids, pkgName);
                SigningDetails signing = verified.get(pkgName);
                if (active && !Objects.equals(shared, oldGroup)) {
                    throw new SecurityException("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE: " + pkgName);
                }
                if (active && old != SigningDetails.UNKNOWN
                        && (signing == SigningDetails.UNKNOWN
                        || !signing.checkCapability(old, SigningDetails.CertCapabilities.INSTALLED_DATA))) {
                    throw new SecurityException("INSTALL_FAILED_UPDATE_INCOMPATIBLE: " + pkgName);
                }
                // A fresh install after removal may choose a new shared identity,
                // but cannot keep the former identity's UID.
                if (ids.containsKey(pkgName) && !Objects.equals(shared, oldGroup)) ids.remove(pkgName);
                if (shared == null) ids.remove("shared-pkg:" + pkgName);
                else ids.setProperty("shared-pkg:" + pkgName, shared);
                writeSigner(ids, pkgName, signing);
                ids.setProperty("active:" + pkgName, "true");
            }
            Map<String, Map<String, SigningDetails>> groups = new HashMap<>();
            for (String key : ids.stringPropertyNames()) {
                if (!key.startsWith("shared-pkg:")) continue;
                String peerName = key.substring("shared-pkg:".length());
                if (!"true".equals(ids.getProperty("active:" + peerName))) continue;
                SigningDetails signing = verified.get(peerName);
                if (signing == null) signing = readSigner(ids, peerName);
                if (signing == SigningDetails.UNKNOWN) throw new SecurityException("Shared UID has no verified owner");
                groups.computeIfAbsent(ids.getProperty(key), unused -> new LinkedHashMap<>()).put(peerName, signing);
            }
            Map<String, SigningDetails> previousShared = new HashMap<>();
            for (ApkPackage pkg : pending) {
                String shared = group(pkg.sharedUserId());
                if (shared == null) continue;
                List<SigningDetails> peers = new ArrayList<>();
                SigningDetails combined = previousShared.computeIfAbsent(shared,
                        value -> sharedSigner(admission.before, value));
                if (combined == SigningDetails.UNKNOWN) combined = null;
                for (Map.Entry<String, SigningDetails> peer : groups.get(shared).entrySet()) {
                    if (peer.getKey().equals(pkg.packageName)) continue;
                    peers.add(peer.getValue());
                    combined = combined == null ? peer.getValue() : combined.mergeLineageWith(peer.getValue(),
                            SigningDetails.CapabilityMergeRule.MERGE_RESTRICTED_CAPABILITY);
                }
                boolean updating = "true".equals(admission.before.getProperty("active:" + pkg.packageName))
                        || current.containsKey(pkg.packageName);
                if (combined != null && !canJoin(verified.get(pkg.packageName), combined, peers, updating)) {
                    throw new SecurityException("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE: " + pkg.packageName);
                }
            }
            Map<String, SigningDetails> updatedGroups = new LinkedHashMap<>();
            for (ApkPackage pkg : pending) {
                String shared = group(pkg.sharedUserId());
                if (shared == null) continue;
                SigningDetails before = updatedGroups.getOrDefault(shared, previousShared.get(shared));
                SigningDetails incomingSigning = verified.get(pkg.packageName);
                // ReconcilePackageUtils lets the incoming lineage update ancestor
                // capabilities, then intersects them with the other current members.
                updatedGroups.put(shared, before == SigningDetails.UNKNOWN ? incomingSigning
                        : before.mergeLineageWith(incomingSigning));
            }
            for (Map.Entry<String, SigningDetails> entry : updatedGroups.entrySet()) {
                SigningDetails signing = entry.getValue();
                for (SigningDetails peer : groups.get(entry.getKey()).values()) {
                    signing = signing.mergeLineageWith(peer,
                            SigningDetails.CapabilityMergeRule.MERGE_RESTRICTED_CAPABILITY);
                }
                writeSigner(ids, "shared:" + entry.getKey(), signing);
            }
            for (ApkPackage pkg : pending) {
                String shared = group(pkg.sharedUserId());
                String stored = ids.getProperty(pkg.packageName);
                int uid;
                if (stored != null) uid = Integer.parseInt(stored);
                else if (shared != null && ids.containsKey("shared:" + shared)) uid = Integer.parseInt(ids.getProperty("shared:" + shared));
                else uid = allocate(ids);
                if (shared != null) {
                    String groupUid = ids.getProperty("shared:" + shared);
                    if (groupUid != null && Integer.parseInt(groupUid) != uid) throw new SecurityException("Shared identity UID changed");
                    ids.setProperty("shared:" + shared, Integer.toString(uid));
                }
                ids.setProperty(pkg.packageName, Integer.toString(uid));
                admission.assignments.put(pkg, uid);
            }
            validate(ids);
            return admission;
        } catch (RuntimeException failure) {
            admission.close(); throw failure;
        }
    }

    private static boolean canJoin(SigningDetails candidate, SigningDetails shared,
            List<SigningDetails> peers, boolean updating) {
        if (!candidate.hasCommonAncestor(shared)) return false;
        boolean capability = candidate.checkCapability(shared, SigningDetails.CertCapabilities.SHARED_USER_ID)
                || shared.checkCapability(candidate, SigningDetails.CertCapabilities.SHARED_USER_ID);
        if (updating && capability) return true;
        if (!capability) {
            // An existing member may rotate away from an ancestor. A new install,
            // or an old signer trying to rejoin, has no such authority.
            return updating && candidate.hasAncestor(shared);
        }
        for (SigningDetails peer : peers) {
            if (candidate.hasAncestor(peer)
                    && !candidate.checkCapability(peer, SigningDetails.CertCapabilities.SHARED_USER_ID)) return false;
        }
        return true;
    }

    static Admission prepareRetire(ApkPackage pkg) {
        Admission admission = new Admission();
        try {
            if (pkg == null || !"true".equals(admission.after.getProperty("active:" + pkg.packageName))) return admission;
            if (!Integer.toString(pkg.uid()).equals(admission.after.getProperty(pkg.packageName))) {
                throw new IllegalStateException("Retiring package identity changed");
            }
            String shared = group(admission.after.getProperty("shared-pkg:" + pkg.packageName));
            SigningDetails sharedSigning = shared == null ? SigningDetails.UNKNOWN
                    : sharedSigner(admission.after, shared);
            admission.after.remove("active:" + pkg.packageName);
            admission.after.remove("signer:" + pkg.packageName);
            admission.after.remove("lineage:" + pkg.packageName);
            if (shared != null) {
                boolean survives = false;
                for (String key : admission.after.stringPropertyNames()) {
                    if (key.startsWith("shared-pkg:") && shared.equals(admission.after.getProperty(key))
                            && "true".equals(admission.after.getProperty("active:"
                                    + key.substring("shared-pkg:".length())))) {
                        survives = true;
                        break;
                    }
                }
                // AOSP removes the SharedUserSetting only with its final package.
                // Keep the app-ID tombstone, but reset signing for a later group lifetime.
                writeSigner(admission.after, "shared:" + shared,
                        survives ? sharedSigning : SigningDetails.UNKNOWN);
            }
            admission.removals.add(pkg.packageName);
            return admission;
        } catch (RuntimeException failure) {
            admission.close(); throw failure;
        }
    }

    private static int allocate(Properties ids) {
        int next = Integer.parseInt(ids.getProperty(NEXT, "10000"));
        if (next > 19999) throw new IllegalStateException("No application UIDs available");
        ids.setProperty(NEXT, Integer.toString(next + 1));
        return next;
    }

    private static void writeSigner(Properties ids, String name, SigningDetails signing) {
        ids.remove("signer:" + name); ids.remove("lineage:" + name);
        if (signing == SigningDetails.UNKNOWN) return;
        ids.setProperty("signer:" + name, encode(signing.getSignatures()));
        Signature[] past = signing.getPastSigningCertificates();
        if (past != null && past.length != 0) ids.setProperty("lineage:" + name, encode(past));
    }
    private static String encode(Signature[] signatures) {
        StringBuilder encoded = new StringBuilder();
        for (Signature signature : signatures) {
            if (encoded.length() != 0) encoded.append(',');
            encoded.append(signature.toCharsString()).append('@').append(signature.getFlags());
        }
        return encoded.toString();
    }
    private static Signature[] decode(String encoded) {
        if (encoded == null) return null;
        String[] values = encoded.split(",", -1);
        Signature[] result = new Signature[values.length];
        for (int i = 0; i < values.length; i++) {
            int separator = values[i].lastIndexOf('@');
            if (separator <= 0) throw new IllegalStateException("Corrupt package signer registry");
            result[i] = new Signature(values[i].substring(0, separator));
            result[i].setFlags(Integer.parseInt(values[i].substring(separator + 1)));
        }
        return result;
    }
    private static SigningDetails readSigner(Properties ids, String name) {
        Signature[] current = decode(ids.getProperty("signer:" + name));
        if (current == null) return SigningDetails.UNKNOWN;
        try {
            return new SigningDetails(current, SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3,
                    decode(ids.getProperty("lineage:" + name)));
        } catch (java.security.cert.CertificateException invalid) {
            throw new IllegalStateException("Corrupt package signer registry", invalid);
        }
    }

    private static void validate(Properties ids) {
        Map<String, Integer> groups = new HashMap<>();
        Set<Integer> used = new HashSet<>();
        int next = 10000;
        for (String key : ids.stringPropertyNames()) {
            if (!key.startsWith("shared:")) continue;
            int uid = checkedUid(ids.getProperty(key));
            if (!used.add(uid)) throw new IllegalStateException("Corrupt shared UID registry");
            groups.put(key.substring("shared:".length()), uid);
            next = Math.max(next, uid + 1);
        }
        for (String key : ids.stringPropertyNames()) {
            if (key.indexOf(':') >= 0 || key.equals(NEXT)) continue;
            name(key);
            int uid = checkedUid(ids.getProperty(key));
            String shared = ids.getProperty("shared-pkg:" + key);
            if (shared != null) {
                if (!Objects.equals(groups.get(shared), uid)) throw new IllegalStateException("Corrupt shared UID member");
            } else if (!used.add(uid)) throw new IllegalStateException("Corrupt package UID registry");
            next = Math.max(next, uid + 1);
        }
        for (String key : ids.stringPropertyNames()) {
            if (key.startsWith("shared-pkg:") && !ids.containsKey(key.substring("shared-pkg:".length()))) {
                throw new IllegalStateException("Orphan shared UID member");
            }
        }
        int storedNext = Integer.parseInt(ids.getProperty(NEXT, Integer.toString(next)));
        if (storedNext < next || storedNext > 20000) throw new IllegalStateException("Corrupt next application UID");
        ids.setProperty(NEXT, Integer.toString(storedNext));
    }
    private static int checkedUid(String value) {
        int uid = Integer.parseInt(value);
        if (uid < 10000 || uid > 19999) throw new IllegalStateException("Corrupt package UID registry");
        return uid;
    }

    /** A file-locked identity transaction, rolled back unless publication accepts it. */
    static final class Admission implements AutoCloseable {
        final Properties before = new Properties();
        final Properties after = new Properties();
        final Map<ApkPackage, Integer> assignments = new LinkedHashMap<>();
        final Set<String> removals = new HashSet<>();
        private File directory, registry, journal, receipt;
        private String transactionId;
        private FileOutputStream lockStream;
        private FileLock fileLock;
        private boolean locked, committed, accepted, closed;
        Admission() { this(true); }
        Admission(boolean persistent) {
            if (!persistent) return;
            REGISTRY_LOCK.lock(); locked = true;
            try {
                directory = new File(Device.dataDir(), "system");
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("Cannot create identity directory");
                registry = new File(directory, "package-uids");
                journal = new File(directory, "package-uids.pending");
                receipt = new File(directory, "package-uids.accepted");
                lockStream = new FileOutputStream(new File(directory, "package-uids.lock"), true);
                fileLock = lockStream.getChannel().lock();
                recover();
                if (registry.isFile()) try (FileInputStream input = new FileInputStream(registry)) { before.load(input); }
                after.putAll(before); validate(after);
            } catch (Exception failure) {
                close(); throw new IllegalStateException("Cannot read persistent application identities", failure);
            }
        }
        void commit() {
            if (closed || committed) throw new IllegalStateException("Identity transaction already completed");
            if (registry != null && !before.equals(after)) {
                transactionId = java.util.UUID.randomUUID().toString();
                Properties pending = new Properties();
                pending.setProperty("id", transactionId);
                for (String key : before.stringPropertyNames()) pending.setProperty("before." + key, before.getProperty(key));
                for (ApkPackage pkg : assignments.keySet()) pending.setProperty("expected." + pkg.packageName, pkg.apkPath);
                for (String name : removals) pending.setProperty("removed." + name, "true");
                writeTo(journal, pending);
                writeTo(registry, after);
            }
            committed = true;
            for (Map.Entry<ApkPackage, Integer> entry : assignments.entrySet()) entry.getKey().assignUid(entry.getValue());
        }
        void accept() {
            if (!committed) throw new IllegalStateException("Identity transaction has not committed");
            if (transactionId != null) {
                Properties proof = new Properties(); proof.setProperty("id", transactionId);
                writeTo(receipt, proof);
            }
            accepted = true;
            cleanupJournal();
        }
        /** packages.list already durably proves this identity publication. */
        void acceptPublished() {
            if (!committed) throw new IllegalStateException("Identity transaction has not committed");
            // A receipt write failure after packages.list succeeds must not roll the
            // identity back under an already-published package. Recovery can verify
            // the journal's exact APK generation (or uninstall) from packages.list.
            accepted = true;
            cleanupJournal();
        }

        private void writeTo(File target, Properties values) {
            File temporary = null;
            try {
                temporary = File.createTempFile("package-uids-", ".tmp", directory);
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    values.store(output, "Gesher admitted identities; allocated app IDs are not reused");
                    output.getFD().sync();
                }
                Files.move(temporary.toPath(), target.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (Exception failure) {
                throw new IllegalStateException("Cannot persist application identities", failure);
            } finally { if (temporary != null) temporary.delete(); }
        }

        private static Properties read(File file) throws java.io.IOException {
            Properties values = new Properties();
            if (file.isFile()) try (FileInputStream input = new FileInputStream(file)) { values.load(input); }
            return values;
        }

        /** Recover a crash between the UID snapshot and packages.list publication. */
        private void recover() throws java.io.IOException {
            if (!journal.isFile()) return;
            Properties pending = read(journal);
            String id = pending.getProperty("id");
            if (id == null) throw new IllegalStateException("Invalid identity transaction journal");
            boolean published = id.equals(read(receipt).getProperty("id"));
            if (!published) {
                Map<String, String> installed = new HashMap<>();
                File packages = new File(directory, "packages.list");
                if (packages.isFile()) for (String line : Files.readAllLines(packages.toPath())) {
                    if (line.startsWith("#")) continue;
                    String[] fields = line.split("\t");
                    if (fields.length >= 4) installed.put(fields[0], new File(fields[1], "base.apk").getCanonicalPath());
                }
                boolean hasExpectation = false, matches = true;
                for (String key : pending.stringPropertyNames()) {
                    if (key.startsWith("expected.")) {
                        hasExpectation = true;
                        matches &= Objects.equals(installed.get(key.substring("expected.".length())),
                                new File(pending.getProperty(key)).getCanonicalPath());
                    } else if (key.startsWith("removed.")) {
                        hasExpectation = true;
                        matches &= !installed.containsKey(key.substring("removed.".length()));
                    }
                }
                published = hasExpectation && matches;
            }
            if (!published) {
                Properties previous = new Properties();
                for (String key : pending.stringPropertyNames()) {
                    if (key.startsWith("before.")) previous.setProperty(key.substring("before.".length()), pending.getProperty(key));
                }
                writeTo(registry, previous);
            }
            // Do not start a new transaction if its recovery evidence cannot be retired.
            Files.deleteIfExists(journal.toPath());
            Files.deleteIfExists(receipt.toPath());
        }

        private void cleanupJournal() {
            if (journal == null || transactionId == null) return;
            try {
                Files.deleteIfExists(journal.toPath());
                Files.deleteIfExists(receipt.toPath());
            } catch (java.io.IOException failure) {
                // A durable receipt keeps an accepted transaction distinguishable from
                // one interrupted before publication, even if Windows retains a file.
                SystemServer.log("Identity journal cleanup deferred: " + failure);
            }
        }
        @Override public void close() {
            if (closed) return;
            closed = true;
            try {
                if (committed && !accepted) {
                    try {
                        if (registry != null && !before.equals(after)) writeTo(registry, before);
                        cleanupJournal();
                    }
                    finally { for (ApkPackage pkg : assignments.keySet()) pkg.assignUid(-1); }
                }
            } finally {
                try { if (fileLock != null) fileLock.release(); }
                catch (java.io.IOException failure) { SystemServer.log("Identity lock release failed: " + failure); }
                try { if (lockStream != null) lockStream.close(); }
                catch (java.io.IOException failure) { SystemServer.log("Identity stream close failed: " + failure); }
                if (locked) { locked = false; REGISTRY_LOCK.unlock(); }
            }
        }
    }
}
