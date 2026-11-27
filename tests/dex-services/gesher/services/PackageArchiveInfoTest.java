/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.res.AssetManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.RandomAccessFile;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.Comparator;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.security.MessageDigest;

/** Actual framework archive parsing and signature verification, as an APK store uses it. */
public final class PackageArchiveInfoTest {
    private static int checks;
    private static final String OLD =
            "6f6811249a2eaa37f4f64fdd29f5bea366feee2edcbe19eb96f6c7f89993d853";
    private static final String NEW =
            "98f19a71a5c3a23864b2d7ed3d7b7132239cf973af934cd0c8beeba5906d06d7";

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static String digest(Signature signature) throws Exception {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())) {
            hex.append(String.format("%02x", b));
        }
        return hex.toString();
    }

    private static Object incremental(String name, Class<?> parameter, Object value) throws Exception {
        Class<?> type = Class.forName("android.os.incremental.IncrementalManager");
        Method method = parameter == null ? type.getDeclaredMethod(name)
                : type.getDeclaredMethod(name, parameter);
        method.setAccessible(true);
        return parameter == null ? method.invoke(null) : method.invoke(null, value);
    }

    private static void signed(PackageManager packages, File apk, boolean rotated) throws Exception {
        PackageInfo legacy = packages.getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNATURES);
        require(legacy != null && "gesher.test.rotated".equals(legacy.packageName),
                "Signed archive did not return its actual manifest: " + apk);
        require(legacy.signatures != null && legacy.signatures.length == 1
                        && OLD.equals(digest(legacy.signatures[0])),
                "Archive GET_SIGNATURES lost its verified oldest signer: " + apk);
        PackageInfo current = packages.getPackageArchiveInfo(apk.getPath(),
                PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES));
        require(current != null && current.signingInfo != null,
                "Archive GET_SIGNING_CERTIFICATES did not return verified signers: " + apk);
        require(current.signingInfo.getApkContentsSigners().length == 1
                        && (rotated ? NEW : OLD).equals(digest(current.signingInfo.getApkContentsSigners()[0])),
                "Archive current signer disagrees with its signed content: " + apk);
        require(current.signingInfo.hasPastSigningCertificates() == rotated,
                "Archive rotation history disagrees with the signing block: " + apk);
        if (rotated) {
            Signature[] history = current.signingInfo.getSigningCertificateHistory();
            require(history.length == 2 && OLD.equals(digest(history[0])) && NEW.equals(digest(history[1])),
                    "Archive rotation lineage is missing or reordered");
        }
    }

    private static Object verityCall(String name, Class<?>[] parameters, Object... values) throws Exception {
        Method method = Class.forName("com.android.internal.security.VerityUtils")
                .getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method.invoke(null, values);
    }

    private static void verity(File existing, File missing) throws Exception {
        require(Boolean.FALSE.equals(verityCall("isFsVeritySupported", new Class<?>[0])),
                "Kernel fs-verity protection falsely advertised");
        for (File file : new File[] {existing, missing}) {
            require(Boolean.FALSE.equals(verityCall("hasFsverity", new Class<?>[] {String.class}, file.getPath())),
                    "File falsely reported protected by fs-verity");
            require(verityCall("getFsverityDigest", new Class<?>[] {String.class}, file.getPath()) == null,
                    "Kernel-enforced file digest fabricated");
            byte[] buffer = new byte[32]; Arrays.fill(buffer, (byte) 0x5a);
            byte[] before = buffer.clone();
            int measure = (int) verityCall("measureFsverityNative", new Class<?>[] {String.class, byte[].class},
                    file.getPath(), buffer);
            require(measure == -(file == existing ? android.system.OsConstants.EOPNOTSUPP : android.system.OsConstants.ENOENT),
                    "Wrong fs-verity measure errno: " + measure);
            require(Arrays.equals(before, buffer), "Failed measure wrote a digest");
            require((int) verityCall("statxForFsverityNative", new Class<?>[] {String.class}, file.getPath())
                    == (file == existing ? 0 : -android.system.OsConstants.ENOENT), "Wrong fs-verity stat errno");
            require((int) verityCall("enableFsverityNative", new Class<?>[] {String.class}, file.getPath())
                    == (file == existing ? android.system.OsConstants.EOPNOTSUPP : android.system.OsConstants.ENOENT),
                    "Wrong fs-verity enable errno");
            try {
                verityCall("setUpFsverity", new Class<?>[] {String.class}, file.getPath());
                throw new AssertionError("fs-verity enable falsely succeeded");
            } catch (InvocationTargetException failure) {
                require(failure.getCause() instanceof IOException, "Wrong fs-verity failure: " + failure.getCause());
            }
        }
        try (FileInputStream stream = new FileInputStream(existing)) {
            int fd = (int) java.io.FileDescriptor.class.getMethod("getInt$").invoke(stream.getFD());
            require((int) verityCall("enableFsverityForFdNative", new Class<?>[] {int.class}, fd)
                    == android.system.OsConstants.EOPNOTSUPP, "Existing fd falsely protected");
            try {
                verityCall("setUpFsverity", new Class<?>[] {int.class}, fd);
                throw new AssertionError("fs-verity fd enable falsely succeeded");
            } catch (InvocationTargetException failure) {
                require(failure.getCause() instanceof IOException, "Wrong fd enable failure");
            }
        }
        require((int) verityCall("enableFsverityForFdNative", new Class<?>[] {int.class}, -1)
                == android.system.OsConstants.EBADF, "Invalid fd did not report EBADF");
        System.out.println("PackageArchiveInfoTest: absent kernel integrity has honest capabilities and failure results");
    }

    private static void sidecar(PackageManager packages, File source, File scratch) throws Exception {
        File apk = new File(scratch, "sidecar.apk");
        Files.copy(source.toPath(), apk.toPath());
        // Parseable v4 metadata, deliberately without a cryptographic signature. The
        // kernel has enforced no Merkle tree, so it cannot establish trust in this file.
        byte[] hashing = ByteBuffer.allocate(45).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(1).put((byte) 12).putInt(0).putInt(32).put(new byte[32]).array();
        byte[] signing = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).array();
        byte[] sidecar = ByteBuffer.allocate(12 + hashing.length + signing.length).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(2).putInt(hashing.length).put(hashing).putInt(signing.length).put(signing).array();
        Files.write(new File(apk.getPath() + ".idsig").toPath(), sidecar);
        Class<?> v4 = Class.forName("android.os.incremental.V4Signature");
        Object parsed = v4.getMethod("readFrom", byte[].class).invoke(null, (Object) sidecar);
        require(Boolean.TRUE.equals(v4.getMethod("isVersionSupported").invoke(parsed)), "Invalid v4 fixture version");
        Class.forName("android.os.incremental.V4Signature$HashingInfo").getMethod("fromByteArray", byte[].class)
                .invoke(null, (Object) hashing);
        Class.forName("android.os.incremental.V4Signature$SigningInfos").getMethod("fromByteArray", byte[].class)
                .invoke(null, (Object) signing);
        try {
            Class.forName("android.util.apk.ApkSignatureSchemeV4Verifier")
                    .getMethod("extractSignature", String.class).invoke(null, apk.getPath());
            throw new AssertionError("Unprotected sidecar treated as kernel-verified v4 metadata");
        } catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            require(cause instanceof SecurityException || cause.getClass().getSimpleName().equals("SignatureNotFoundException"),
                    "Sidecar failed outside supported absence/security contract: " + cause);
        }
        // AOSP's feature flag controls whether .idsig is inspected. Either reject the
        // unprotected v4 sidecar or independently verify v2; never trust its empty signer.
        PackageInfo info = packages.getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNATURES);
        require(info == null || (info.signatures != null && info.signatures.length == 1
                && OLD.equals(digest(info.signatures[0]))), "Sidecar bypassed APK signer verification");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the signing fixture directory");
        System.loadLibrary("android_runtime");
        AssetManager.class.getMethod("createSystemAssetsInZygoteLocked", boolean.class, String.class)
                .invoke(null, false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        // Archive parsing is implemented in PackageManager, entirely locally. Use the real
        // ApplicationPackageManager; this manifest needs neither a Context nor a PM Binder.
        Constructor<?> constructor = Class.forName("android.app.ApplicationPackageManager")
                .getDeclaredConstructor(Class.forName("android.app.ContextImpl"),
                        Class.forName("android.content.pm.IPackageManager"));
        constructor.setAccessible(true);
        PackageManager packages = (PackageManager) constructor.newInstance(null, null);
        File fixtures = new File(args[0]);
        File v2 = new File(fixtures, "v2only.apk");
        File rotated = new File(fixtures, "rotated.apk");
        signed(packages, v2, false);
        signed(packages, rotated, true);
        System.out.println("PackageArchiveInfoTest: signed v2 and rotated v3 archives retain verified identities");

        File scratch = Files.createTempDirectory("archive-info-").toFile();
        try {
            File changed = new File(scratch, "tampered.apk");
            Files.copy(v2.toPath(), changed.toPath());
            try (RandomAccessFile file = new RandomAccessFile(changed, "rw")) {
                require(file.readInt() == 0x504b0304, "Unexpected fixture ZIP prefix");
                // Alter only the ZIP local-header timestamp: parsing still works, digest does not.
                file.seek(10); int value = file.readUnsignedByte();
                file.seek(10); file.writeByte(value ^ 1);
            }
            require(packages.getPackageArchiveInfo(changed.getPath(), 0) != null,
                    "Tamper fixture no longer exercises valid manifest parsing");
            for (File rejected : new File[] {changed, new File(fixtures, "unsigned.apk")}) {
                require(packages.getPackageArchiveInfo(rejected.getPath(), PackageManager.GET_SIGNATURES) == null,
                        "Archive returned signer identity for unsigned or changed content: " + rejected);
                require(packages.getPackageArchiveInfo(rejected.getPath(), PackageManager.PackageInfoFlags.of(
                        PackageManager.GET_SIGNING_CERTIFICATES)) == null,
                        "Typed archive flags accepted unsigned or changed content: " + rejected);
            }
            File malformed = new File(scratch, "invalid.apk");
            Files.writeString(malformed.toPath(), "not an APK");
            require(packages.getPackageArchiveInfo(malformed.getPath(), PackageManager.GET_SIGNATURES) == null,
                    "Malformed archive accepted");
            require(packages.getPackageArchiveInfo(new File(scratch, "missing.apk").getPath(), 0) == null,
                    "Missing archive accepted");
            System.out.println("PackageArchiveInfoTest: tampered, unsigned, malformed and missing archives rejected");

            require(Boolean.FALSE.equals(incremental("nativeIsEnabled", null, null)), "IncFS falsely enabled");
            require(Boolean.FALSE.equals(incremental("nativeIsV2Available", null, null)), "IncFS v2 falsely enabled");
            require(Boolean.FALSE.equals(incremental("nativeIsIncrementalPath", String.class, v2.getPath())),
                    "Regular Windows file reported incremental");
            require(incremental("nativeUnsafeGetFileSignature", String.class, v2.getPath()) == null,
                    "Regular Windows file returned an IncFS signature");
            try (FileInputStream file = new FileInputStream(v2)) {
                require(Boolean.FALSE.equals(incremental("nativeIsIncrementalFd", int.class,
                        (int) java.io.FileDescriptor.class.getMethod("getInt$").invoke(file.getFD()))),
                        "Regular Windows descriptor reported incremental");
            }
            verity(v2, new File(scratch, "missing.apk"));
            sidecar(packages, v2, scratch);
        } finally {
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(scratch.toPath())) {
                for (java.nio.file.Path path : (Iterable<java.nio.file.Path>) paths.sorted(Comparator.reverseOrder())::iterator) {
                    Files.delete(path);
                }
            }
        }
        System.out.println("PackageArchiveInfoTest all passed: " + checks + " checks");
    }
}
