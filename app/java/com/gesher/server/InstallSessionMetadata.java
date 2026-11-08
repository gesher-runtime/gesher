/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.Checksum;
import android.os.PersistableBundle;
import java.io.*;
import java.nio.file.Files;
import java.security.cert.Certificate;
import java.util.*;
import org.xmlpull.v1.XmlPullParser;
import sun.security.pkcs.PKCS7;
import sun.security.pkcs.SignerInfo;

/** Session-only, non-executable auxiliary records. They never establish APK signing identity. */
final class InstallSessionMetadata {
    static final String APP_METADATA = "app.metadata";
    static final int MAX_METADATA = 32_000, MAX_SIGNATURE = 35 * 1024;
    private final List<File> files;
    private final List<File> compared;
    InstallSessionMetadata(File canonical) {
        File[] found = canonical.listFiles((dir, name) -> auxiliary(name));
        files = found == null ? Collections.emptyList() : Arrays.asList(found);
        File[] all = canonical.listFiles((dir, name) -> auxiliary(name) || name.endsWith(".dm"));
        compared = all == null ? Collections.emptyList() : Arrays.asList(all);
    }
    static boolean auxiliary(String name) {
        return APP_METADATA.equals(name) || name.endsWith(".digests")
                || name.endsWith(".digests.signature");
    }
    static String validName(String name) {
        // AOSP accepts arbitrary installer names and renames parsed APKs at commit.
        // Windows additionally aliases case, trailing dots and DOS device names.
        if (name == null || !name.matches("[A-Za-z0-9_-][A-Za-z0-9_.-]{0,199}")
                || name.endsWith(".") || name.toUpperCase(Locale.ROOT).split("\\.", 2)[0]
                    .matches("CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]")) {
            throw new IllegalArgumentException("Invalid installation filename");
        }
        return name;
    }
    static String category(String name) {
        if (name == null) return "OTHER";
        if (name.endsWith(".apk")) return "APK";
        if (name.endsWith(".dm")) return "DM";
        if (name.endsWith(".idsig")) return "IDSIG";
        return "OTHER";
    }
    static String dexName(String apk) {
        return apk.endsWith(".apk") ? apk.substring(0, apk.length() - 4) + ".dm" : apk + ".dm";
    }
    static void validateMetadata(File file) throws Exception {
        if (!file.isFile() || file.length() > MAX_METADATA) throw new IOException("App metadata size limit");
        byte[] bytes = Files.readAllBytes(file.toPath());
        // Validate depth before PersistableBundle recursively constructs nested bundles.
        XmlPullParser parser = android.util.Xml.resolvePullParser(new ByteArrayInputStream(bytes));
        int events = 0;
        boolean root = false, closed = false;
        int event;
        while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
            if (++events > 4096 || parser.getDepth() > 32) throw new IOException("App metadata nesting limit");
            if (event == XmlPullParser.START_TAG && parser.getDepth() == 1) {
                if (root || !"bundle".equals(parser.getName())) throw new IOException("Invalid app metadata root");
                root = true;
            }
            if (event == XmlPullParser.END_TAG && parser.getDepth() == 1) closed = true;
            if (event == XmlPullParser.TEXT && parser.getDepth() == 0 && !parser.getText().trim().isEmpty()) {
                throw new IOException("Invalid app metadata content");
            }
        }
        if (!root || !closed) throw new IOException("Missing app metadata bundle");
        PersistableBundle.readFromStream(new ByteArrayInputStream(bytes));
    }
    static final class Digests {
        final byte[] bytes, signature;
        Digests(Checksum[] checksums, byte[] suppliedSignature) {
            if (checksums == null || checksums.length > 7) throw new IllegalArgumentException("Invalid checksums");
            try {
                ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                DataOutputStream output = new DataOutputStream(buffer);
                Set<Integer> types = new HashSet<>();
                for (Checksum checksum : checksums) {
                    if (checksum == null) throw new IllegalArgumentException("Missing checksum");
                    int type = checksum.getType();
                    int length = type == 2 ? 16 : type == 4 ? 20 : type == 1 || type == 8 || type == 32 ? 32
                            : type == 16 || type == 64 ? 64 : -1;
                    byte[] value = checksum.getValue();
                    if (value == null || value.length != length || !types.add(type)) {
                        throw new IllegalArgumentException("Invalid checksum type or value");
                    }
                    output.writeInt(type); output.writeInt(value.length); output.write(value);
                }
                bytes = buffer.toByteArray();
                signature = suppliedSignature == null || suppliedSignature.length == 0
                        ? null : suppliedSignature.clone();
                if (signature != null) {
                    if (signature.length > MAX_SIGNATURE) throw new IllegalArgumentException("Checksum signature too large");
                    // Same detached PKCS#7 verification used by AOSP ApkChecksums.
                    PKCS7 pkcs7 = new PKCS7(signature);
                    Certificate[] certs = pkcs7.getCertificates();
                    SignerInfo[] signers = pkcs7.verify(bytes);
                    if (certs == null || certs.length == 0 || signers == null || signers.length == 0) {
                        throw new IllegalArgumentException("Checksum signature verification failed");
                    }
                    for (SignerInfo signer : signers) {
                        if (signer.getCertificateChain(pkcs7) == null) {
                            throw new IllegalArgumentException("Missing checksum signer chain");
                        }
                    }
                }
            } catch (IllegalArgumentException e) { throw e; }
            catch (Exception e) { throw new IllegalArgumentException("Checksum signature verification failed", e); }
        }
        void store(File canonical, String apkName) throws IOException {
            String name = apkName.substring(0, apkName.length() - 4) + ".digests";
            Files.write(new File(canonical, name).toPath(), bytes);
            if (signature != null) Files.write(new File(canonical, name + ".signature").toPath(), signature);
        }
    }
    boolean matches(File installed) throws IOException {
        File[] old = installed.listFiles((dir, name) -> auxiliary(name) || name.endsWith(".dm"));
        if ((old == null ? 0 : old.length) != compared.size()) return false;
        for (File file : compared) {
            File previous = new File(installed, file.getName());
            if (!previous.isFile() || !Arrays.equals(Files.readAllBytes(file.toPath()),
                    Files.readAllBytes(previous.toPath()))) return false;
        }
        return true;
    }
    void copyTo(File installed) throws IOException {
        for (File file : files) Files.copy(file.toPath(), new File(installed, file.getName()).toPath());
    }
}
