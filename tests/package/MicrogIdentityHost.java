/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import com.android.apksig.apk.ApkUtils;
import com.android.apksig.util.DataSources;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.MessageDigestSpi;
import java.security.Provider;
import java.security.Security;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipFile;
import sun.misc.Unsafe;

/** Real signed APK bytes and production identity gates; only Android manifest objects are constructed.
 * The separate ART MicrogTest exercises PackageParser2 and the complete runtime resource boundary. */
public final class MicrogIdentityHost {
    private static final Unsafe UNSAFE;
    private static final AtomicLong HASH_BYTES = new AtomicLong();
    private static int checks;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            UNSAFE = (Unsafe)field.get(null);
        } catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    public static final class CountingDigest extends MessageDigestSpi {
        private final MessageDigest real;
        public CountingDigest() {
            try { real = MessageDigest.getInstance("SHA-256", "SUN"); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Override protected int engineGetDigestLength() { return real.getDigestLength(); }
        @Override protected void engineUpdate(byte value) { HASH_BYTES.incrementAndGet(); real.update(value); }
        @Override protected void engineUpdate(byte[] values, int offset, int length) {
            HASH_BYTES.addAndGet(length); real.update(values, offset, length);
        }
        @Override protected byte[] engineDigest() { return real.digest(); }
        @Override protected void engineReset() { real.reset(); }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message); checks++;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name); field.setAccessible(true); field.set(target, value);
    }
    private static ApkPackage metadata(Path path) throws Exception {
        String name;
        try (RandomAccessFile file = new RandomAccessFile(path.toFile(), "r")) {
            name = ApkUtils.getPackageNameFromBinaryAndroidManifest(
                    ApkUtils.getAndroidManifest(DataSources.asDataSource(file)));
        }
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, path.toString());
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(GmsSignatureSpoofing.PERMISSION_FAKE_PACKAGE_SIGNATURE, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(pkg, "apkPath", path.toString()); set(pkg, "packageName", name); set(pkg, "parsed", parsed);
        // No verified fields are supplied by this fixture. Production apksig must
        // authenticate the actual file, independently of this parsed metadata.
        return pkg;
    }
    private static boolean addon(String expected, ApkPackage pkg) throws Exception {
        // Admission still authenticates current bytes and their actual manifest
        // name, independently of previously parsed or verified object metadata.
        return GmsSignatureSpoofing.verifyAddonApk(expected, pkg.apkPath);
    }
    private static void reusedIdentity(Path companion) throws Exception {
        ApkPackage pkg = metadata(companion);
        HASH_BYTES.set(0);
        Signature[] verified = pkg.verifiedSignatures();
        check(verified != null && verified.length == 1 && HASH_BYTES.get() > Files.size(companion) / 2,
                "The APK generation must first undergo real full-content verification");
        int certificateBytes = verified[0].toByteArray().length;
        HASH_BYTES.set(0);
        check(pkg.hasVerifiedMicrogSigner(), "Official Companion must retain its verified microG identity");
        long additional = HASH_BYTES.get();
        System.out.println("microG object gate: additional SHA-256 input=" + additional
                + " bytes; certificate=" + certificateBytes + " bytes; APK=" + Files.size(companion) + " bytes");
        check(additional == certificateBytes, "The microG gate must hash only the verified certificate, not the APK again: " + additional);
        for (int i = 0; i < 1000; i++) {
            if (!pkg.hasVerifiedMicrogSigner()) throw new AssertionError("Cached verified addon changed");
        }
        check(HASH_BYTES.get() == additional, "Repeated identity checks reuse only this generation's result");
        check(addon("com.android.vending", pkg), "Correct addon slot authenticates the current file");
        check(!addon("com.google.android.gms", pkg), "An official signer does not authorize another package slot");
        check(!addon("fixture.unlisted", pkg) && !addon(null, pkg), "The addon allowlist remains mandatory");
        check(GmsSignatureSpoofing.isTrustedPackage(pkg) == GmsSignatureSpoofing.isEnabled(),
                "Signature compatibility still requires explicit opt-in and requested permission");
        ApkPackage predecessor = metadata(companion); predecessor.setSystemAddon(true);
        List<ApkPackage> selection = new ArrayList<>(List.of(predecessor));
        SystemServer.selectCommandLinePackage(selection, pkg);
        check(selection.size() == 1 && selection.get(0) == pkg && pkg.isSystemAddon(),
                "Production command-line addon replacement authenticates the current file");
    }
    private static void corruptDex(Path path) throws Exception {
        long compressed;
        try (ZipFile zip = new ZipFile(path.toFile())) { compressed = zip.getEntry("classes.dex").getCompressedSize(); }
        byte[] bytes = Files.readAllBytes(path);
        ByteBuffer little = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        for (int offset = 0; offset + 30 < bytes.length; offset++) {
            if (little.getInt(offset) != 0x04034b50) continue;
            int nameLength = Short.toUnsignedInt(little.getShort(offset + 26));
            int extraLength = Short.toUnsignedInt(little.getShort(offset + 28));
            if (offset + 30 + nameLength > bytes.length) continue;
            String name = new String(bytes, offset + 30, nameLength, java.nio.charset.StandardCharsets.UTF_8);
            if (!name.equals("classes.dex")) continue;
            int payload = Math.toIntExact(offset + 30L + nameLength + extraLength + compressed / 2);
            bytes[payload] ^= 1;
            Files.write(path, bytes); return;
        }
        throw new AssertionError("No dex payload to corrupt");
    }
    private static void replacement(Path companion, Path directory) throws Exception {
        Path mutable = directory.resolve("same-path.apk");
        Files.copy(companion, mutable, StandardCopyOption.REPLACE_EXISTING);
        ApkPackage original = metadata(mutable);
        check(original.hasVerifiedMicrogSigner(), "The original file is a valid official addon");
        var modified = Files.getLastModifiedTime(mutable);
        long length = Files.size(mutable);
        corruptDex(mutable); Files.setLastModifiedTime(mutable, modified);
        check(Files.size(mutable) == length && Files.getLastModifiedTime(mutable).equals(modified),
                "Tampering deliberately preserves both path metadata shortcuts");
        ApkPackage changed = metadata(mutable);
        // An unverified compatibility parser may still expose the old certificate.
        changed.parsed.setSigningDetails(new SigningDetails(original.verifiedSignatures(),
                SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2));
        check(!changed.hasVerifiedMicrogSigner() && changed.verifiedSignatures() == null,
                "New generation rejects changed content despite the same path, size, mtime and parsed certificate");
        check(!addon("com.android.vending", changed), "Admission rejects corrupted signed content");
        check(!GmsSignatureSpoofing.verifyAddonApk("com.android.vending", mutable.toString()),
                "The unchanged standalone pre-parse gate also rejects tampering");
        List<ApkPackage> selected = new ArrayList<>(List.of(original)); original.setSystemAddon(true);
        boolean denied = false;
        try { SystemServer.selectCommandLinePackage(selected, changed); }
        catch (SecurityException expected) { denied = true; }
        check(denied && selected.size() == 1 && selected.get(0) == original && !changed.isSystemAddon(),
                "Rejected command-line replacement preserves the preceding installed selection");
        // This must fail at the deliberate pre-parse gate. The host has no
        // AssetManager native implementation that could hide parser reordering.
        denied = false;
        try { MicrogAddons.load(mutable.toFile(), "com.android.vending"); }
        catch (SecurityException expected) { denied = true; }
        check(denied, "Bundled addon still rejects invalid content before PackageParser2");
        Files.copy(companion, mutable, StandardCopyOption.REPLACE_EXISTING);
        check(metadata(mutable).hasVerifiedMicrogSigner(), "A later valid generation does not inherit a rejected object's cache");
    }
    private static void legacyPin(Path gsf, Path directory) throws Exception {
        check(GmsSignatureSpoofing.verifyAddonApk("com.google.android.gsf", gsf.toString()),
                "The exact official v1 GSF artifact passes the unchanged preflight");
        ApkPackage original = metadata(gsf);
        check(original.hasVerifiedMicrogSigner() && addon("com.google.android.gsf", original),
                "Policy and admission retain the exact-pinned legacy GSF exception");
        byte[] bytes = Files.readAllBytes(gsf);
        ByteBuffer little = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int end = -1;
        for (int i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
            if (little.getInt(i) == 0x06054b50 && i + 22 + Short.toUnsignedInt(little.getShort(i + 20)) == bytes.length) {
                end = i; break;
            }
        }
        check(end >= 0, "GSF fixture has a valid ZIP end record");
        byte[] commented = java.util.Arrays.copyOf(bytes, bytes.length + 1);
        ByteBuffer.wrap(commented).order(ByteOrder.LITTLE_ENDIAN).putShort(end + 20,
                (short)(Short.toUnsignedInt(little.getShort(end + 20)) + 1));
        commented[commented.length - 1] = '!';
        Path changed = directory.resolve("gsf-valid-signature-different-artifact.apk"); Files.write(changed, commented);
        ApkPackage unpinned = metadata(changed);
        check(unpinned.verifiedSignatures() != null, "ZIP comment change preserves actual v1 signed content and certificate");
        check(!unpinned.hasVerifiedMicrogSigner() && !addon("com.google.android.gsf", unpinned),
                "Valid v1 microG signatures still cannot replace the whole-file pin");
        check(!GmsSignatureSpoofing.verifyOfficialApk(changed.toString()), "Standalone and object legacy policies agree");
    }
    private static void changedManifest(Path companion, Path gsf, Path directory) throws Exception {
        Path mutable = directory.resolve("changed-manifest.apk");
        Files.copy(companion, mutable, StandardCopyOption.REPLACE_EXISTING);
        ApkPackage current = metadata(mutable); // Parse Companion, before verifying any identity.
        Files.copy(gsf, mutable, StandardCopyOption.REPLACE_EXISTING);
        check(GmsSignatureSpoofing.verifyAddonApk("com.google.android.gsf", mutable.toString()),
                "Replacement bytes are the real official, exact-pinned GSF artifact");
        check(!GmsSignatureSpoofing.verifyAddonApk("com.android.vending", mutable.toString()),
                "The existing path gate rejects the changed manifest name");
        ApkPackage previous = metadata(companion); previous.setSystemAddon(true);
        List<ApkPackage> selected = new ArrayList<>(List.of(previous));
        boolean denied = false;
        try { SystemServer.selectCommandLinePackage(selected, current); }
        catch (SecurityException expected) { denied = true; }
        System.out.println("changed manifest: denied=" + denied + ", current selected="
                + (selected.get(0) == current) + ", addon=" + current.isSystemAddon());
        check(denied && selected.size() == 1 && selected.get(0) == previous && !current.isSystemAddon(),
                "An official signer must not substitute another actual package after metadata parsing");
    }
    private static void changedVerifiedContent(Path companion, Path directory) throws Exception {
        Path mutable = directory.resolve("changed-verified-content.apk");
        Files.copy(companion, mutable, StandardCopyOption.REPLACE_EXISTING);
        ApkPackage current = metadata(mutable);
        check(current.verifiedSignatures() != null, "The candidate was fully verified before its bytes changed");
        corruptDex(mutable); // The actual manifest name remains com.android.vending.
        ApkPackage previous = metadata(companion); previous.setSystemAddon(true);
        List<ApkPackage> selected = new ArrayList<>(List.of(previous));
        boolean denied = false;
        try { SystemServer.selectCommandLinePackage(selected, current); }
        catch (SecurityException expected) { denied = true; }
        check(denied && selected.size() == 1 && selected.get(0) == previous && !current.isSystemAddon(),
                "Admission must recheck current file contents even when an earlier identity and manifest name match");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Provider provider = new Provider("GesherMicrogCountSHA256", "1.0", "Delegating real APK digest counter") { };
        provider.put("MessageDigest.SHA-256", CountingDigest.class.getName()); Security.insertProviderAt(provider, 1);
        try {
            Path companion = Path.of(args[1]), gsf = Path.of(args[2]), fixtures = Path.of(args[3]), work = Path.of(args[4]);
            changedManifest(companion, gsf, work);
            changedVerifiedContent(companion, work);
            reusedIdentity(companion); replacement(companion, work); legacyPin(gsf, work);
            for (String name : List.of("v2only.apk", "rotated.apk", "unsigned.apk")) {
                ApkPackage other = metadata(fixtures.resolve(name));
                check(!other.hasVerifiedMicrogSigner(), "Non-microG/unsigned identity remains denied: " + name);
            }
            System.out.println("Microg identity host passed: " + checks + " checks; spoof=" + GmsSignatureSpoofing.isEnabled());
        } finally { Security.removeProvider(provider.getName()); }
    }
}
