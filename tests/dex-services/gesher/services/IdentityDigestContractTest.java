/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.Signature;
import com.gesher.app.shell.AppShell;
import com.gesher.server.ApkPackage;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import java.util.stream.Stream;
import sun.misc.Unsafe;

/** Complete production identity functions, with explicit installed-signature metadata inputs. */
public final class IdentityDigestContractTest {
    private static final Unsafe UNSAFE;
    private static final Method SIGNER;
    private static int checks;
    static {
        try {
            Field f = field(Unsafe.class, "theUnsafe"); UNSAFE = (Unsafe) f.get(null);
            // Do not initialize the production cache before first-call measurements.
            Class<?> store = Class.forName("com.gesher.server.AppOpsModeStore", false,
                    ApkPackage.class.getClassLoader());
            SIGNER = store.getDeclaredMethod("signer", ApkPackage.class); SIGNER.setAccessible(true);
        } catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private static Field field(Class<?> owner, String name) throws Exception {
        Field f = owner.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private interface Action { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> type, Action action) throws Exception {
        try { action.run(); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            check(type.isInstance(error), "Wrong error: " + error); return;
        }
        throw new AssertionError("Expected " + type.getName());
    }
    public static byte[] bytes(int length, int seed) {
        byte[] result = new byte[length];
        for (int i = 0; i < result.length; i++) result[i] = (byte) (i * 73 + seed * 29);
        return result;
    }
    private static ApkPackage pkg(String name, Path file, byte[][] certificates) throws Exception {
        ApkPackage pkg = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(pkg, name);
        field(ApkPackage.class, "apkPath").set(pkg, file.toString());
        field(ApkPackage.class, "mUid").setInt(pkg, 13281);
        field(ApkPackage.class, "mSigningIdentityChecked").setBoolean(pkg, true);
        Signature[] signers = null;
        if (certificates != null) {
            signers = new Signature[certificates.length];
            for (int i = 0; i < signers.length; i++) signers[i] = new Signature(certificates[i]);
        }
        field(ApkPackage.class, "mVerifiedSignatures").set(pkg, signers);
        return pkg;
    }
    private static String signer(ApkPackage pkg) throws Exception {
        try { return (String) SIGNER.invoke(null, pkg); }
        catch (InvocationTargetException e) {
            if (e.getCause() instanceof Exception failure) throw failure;
            if (e.getCause() instanceof Error failure) throw failure;
            throw e;
        }
    }
    // Independent standard BigInteger encoding, not the candidate HexEncoding implementation.
    private static String sha(byte[] input) throws Exception {
        String hex = new BigInteger(1, MessageDigest.getInstance("SHA-256").digest(input)).toString(16);
        return "0".repeat(64 - hex.length()) + hex;
    }
    private static String key(String pkg, String id) throws Exception {
        return sha((pkg + '\0' + id).getBytes(StandardCharsets.UTF_8));
    }
    private static String notification(byte[][] certificates) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream data = new DataOutputStream(bytes)) {
            for (byte[] certificate : certificates) { data.writeInt(certificate.length); data.write(certificate); }
        }
        return "cert:" + sha(bytes.toByteArray());
    }
    private static String appOps(byte[][] certificates) throws Exception {
        if (certificates == null || certificates.length == 0) return null;
        List<String> hashes = new ArrayList<>();
        for (byte[] certificate : certificates) hashes.add(sha(certificate));
        Collections.sort(hashes); return String.join(":", hashes);
    }
    private static String files(List<Path> orderedFiles) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream data = new DataOutputStream(bytes)) {
            for (Path file : orderedFiles) {
                byte[] name = file.getFileName().toString().getBytes(StandardCharsets.UTF_8);
                data.writeInt(name.length); data.write(name);
                data.writeLong(Files.size(file)); data.write(Files.readAllBytes(file));
            }
        }
        return "apk:" + sha(bytes.toByteArray());
    }
    public static int contracts(Path root) throws Exception {
        Files.createDirectories(root);
        for (int value = 0; value < 256; value++) {
            check(libcore.util.HexEncoding.encodeToString(new byte[]{(byte) value}, false)
                    .equals(String.format(Locale.ROOT, "%02x", value)), "shipped libcore two-digit lowercase parity " + value);
        }
        check(libcore.util.HexEncoding.encodeToString(new byte[0], false).isEmpty(), "empty shipped encoder result");
        Locale previous = Locale.getDefault();
        try {
            String[] ids = {"", "one", "a\0b", "משימה/📞\"", "\ud800", "x".repeat(8192)};
            for (Locale locale : List.of(Locale.ROOT, Locale.US, Locale.forLanguageTag("ar-EG"),
                    Locale.forLanguageTag("tr-TR"), Locale.forLanguageTag("he-IL"))) {
                Locale.setDefault(locale);
                for (String id : ids) {
                    String actual = AppShell.dynamicKey("fixture.identity", id);
                    check(actual.equals(key("fixture.identity", id)), "shortcut exact UTF-8/separator/lowercase identity");
                    check(AppShell.dynamicArguments("fixture.identity", id).equals("launch fixture.identity --shortcut " + actual),
                            "shortcut arguments retain the exact opaque key");
                }
            }
        } finally { Locale.setDefault(previous); }
        rejects(IllegalArgumentException.class, () -> AppShell.dynamicKey(null, "id"));
        rejects(NullPointerException.class, () -> AppShell.dynamicKey("fixture.identity", null));
        for (String invalid : List.of("", ".", "a..b", "a-b", "a/b", "a\\b", "a\0b", "שלום")) {
            rejects(IllegalArgumentException.class, () -> AppShell.dynamicKey(invalid, "id"));
        }
        for (String valid : List.of("a", "_", "a._1.b", "fixture.identity")) {
            check(AppShell.dynamicKey(valid, "id").equals(key(valid, "id")), "valid package encoding");
        }
        boolean[] seen = new boolean[256];
        for (int seed = 0; seed < 256; seed++) {
            byte[][] certificates = {bytes(seed + 1, seed)};
            ApkPackage pkg = pkg("fixture.identity." + seed, root.resolve("absent.apk"), certificates);
            String expectedNotification = notification(certificates), expectedSigner = appOps(certificates);
            check(pkg.codeIdentity().equals(expectedNotification), "notification complete signer digest " + seed);
            check(signer(pkg).equals(expectedSigner), "AppOps complete signer digest " + seed);
            String cached = signer(pkg);
            check(cached == signer(pkg), "existing package-instance signer cache retained");
            for (int i = 0; i < 64; i += 2) seen[Integer.parseInt(expectedSigner.substring(i, i + 2), 16)] = true;
        }
        for (boolean value : seen) check(value, "all byte values occur in checked digests");
        byte[] a = bytes(257, 3), b = bytes(1024, 4);
        for (byte[][] certificates : new byte[][][]{new byte[0][], {new byte[0]}, {a, b}, {b, a}, {a, b, a}}) {
            ApkPackage pkg = pkg("fixture.identity.multiple", root.resolve("unused.apk"), certificates);
            check(pkg.codeIdentity().equals(notification(certificates)), "notification signer order/length/prefix preserved");
            check(java.util.Objects.equals(signer(pkg), appOps(certificates)), "AppOps sorted signer list/duplicates/null retained");
        }
        ApkPackage before = pkg("fixture.identity.replaced", root.resolve("unused.apk"), new byte[][]{a});
        ApkPackage after = pkg("fixture.identity.replaced", root.resolve("unused.apk"), new byte[][]{b});
        check(!signer(before).equals(signer(after)), "same-name/same-UID replacement must not inherit cached signer");
        check(!before.codeIdentity().equals(after.codeIdentity()), "notification replacement identity changes");
        Path base = root.resolve("base.apk"), splitZ = root.resolve("split_z.apk"), splitA = root.resolve("split_a.apk");
        Files.write(splitZ, bytes(8193, 7)); Files.write(base, bytes(65537, 9)); Files.write(splitA, bytes(31, 2));
        Files.write(root.resolve("irrelevant.txt"), bytes(51, 1));
        Files.write(root.resolve("split_ignored.bin"), bytes(23, 3));
        ApkPackage unsigned = pkg("fixture.identity.unsigned", base, null);
        List<Path> expectedFiles = List.of(base, splitA, splitZ);
        check(unsigned.codeIdentity().equals(files(expectedFiles)), "actual base/split bytes, sorted names and big-endian lengths");
        check(signer(unsigned) == null, "unsigned code cannot acquire an AppOps signer");
        String original = unsigned.codeIdentity();
        Files.write(splitA, bytes(31, 6));
        check(!unsigned.codeIdentity().equals(original) && unsigned.codeIdentity().equals(files(expectedFiles)), "same-size split replacement changes exact-code identity");
        Files.move(splitA, root.resolve("split_renamed.apk"));
        check(unsigned.codeIdentity().equals(files(List.of(base, root.resolve("split_renamed.apk"), splitZ))), "split filename is part of identity");
        Files.delete(base);
        check(unsigned.codeIdentity() == null, "missing base fails closed");
        Files.createDirectory(base);
        check(unsigned.codeIdentity() == null, "unreadable base directory fails closed");
        Files.delete(base);
        check(signer(unsigned) == null, "cached unsigned state remains unsigned after file changes");
        return checks;
    }

    /** Fixed inputs precede clocks; each invocation executes the complete real operation. */
    public interface Workload { String invoke() throws Exception; default void close() throws Exception { } }
    public interface Signer { String get(ApkPackage pkg) throws Exception; }
    public static Workload prepare(String name, Path root, int ownerCount) throws Exception {
        return prepare(name, root, ownerCount, IdentityDigestContractTest::signer);
    }
    public static Workload prepare(String name, Path root, int ownerCount, Signer signerCall) throws Exception {
        Files.createDirectories(root);
        if (name.startsWith("key-")) {
            String id = name.equals("key-tiny") ? "" : name.equals("key-unicode") ? "משימה/📞\"\0id" : "x".repeat(8192);
            return () -> AppShell.dynamicArguments("fixture.identity", id);
        }
        if (name.startsWith("signed-")) {
            int count = name.equals("signed-empty") ? 0 : name.equals("signed-four") ? 4 : 1;
            byte[][] certs = new byte[count][];
            for (int i = 0; i < count; i++) certs[i] = bytes(1024, i + 1);
            ApkPackage pkg = pkg("fixture.identity", root.resolve("unused.apk"), certs);
            return pkg::codeIdentity;
        }
        if (name.startsWith("unsigned-")) {
            Path base = root.resolve("base.apk");
            int size = name.equals("unsigned-tiny") ? 257 : name.equals("unsigned-large") ? 1048576 : 65537;
            Files.write(base, bytes(size, 9));
            if (name.equals("unsigned-splits")) {
                Files.write(root.resolve("split_z.apk"), bytes(8193, 7));
                Files.write(root.resolve("split_a.apk"), bytes(31, 2));
            }
            ApkPackage pkg = pkg("fixture.identity", base, null);
            return pkg::codeIdentity;
        }
        if (name.startsWith("appops-")) {
            boolean cold = name.equals("appops-new-owner"), unsigned = name.equals("appops-unsigned");
            ApkPackage[] owners = new ApkPackage[cold ? ownerCount : 1];
            for (int i = 0; i < owners.length; i++) owners[i] = pkg("fixture.identity." + i, root.resolve("unused.apk"),
                    unsigned ? null : new byte[][]{bytes(1024, 1), bytes(257, 2)});
            return new Workload() {
                int next;
                public String invoke() throws Exception { return signerCall.get(owners[cold ? next++ : 0]); }
                public void close() throws Exception {
                    if (cold) check(next == owners.length, "every prepared owner processed exactly once");
                }
            };
        }
        if (name.equals("shortcut-publication")) {
            String pkg = "fixture.identity.publication", identity = "cert:identity-publication-fixture";
            return new Workload() {
                public String invoke() throws Exception {
                    check(AppShell.syncDynamicShortcuts(pkg, List.of(), identity), "real empty shortcut publication succeeds");
                    Properties stored = AppShell.dynamicShortcutsOf(pkg);
                    check(identity.equals(stored.getProperty("publisher")) && pkg.equals(stored.getProperty("package")), "real stored publisher retained");
                    check(AppShell.loadDynamicShortcuts(pkg, identity).isEmpty(), "real readback has no fabricated shortcuts");
                    AppShell.removeAllDynamicShortcuts(pkg);
                    check(AppShell.dynamicShortcutsOf(pkg).isEmpty(), "real retirement removes callable publication");
                    return identity;
                }
                public void close() { AppShell.removeAllDynamicShortcuts(pkg); }
            };
        }
        throw new IllegalArgumentException("Unknown workload " + name);
    }

    public static void deleteOwned(Path root) throws Exception {
        try (Stream<Path> walk = Files.walk(root)) {
            for (Path path : (Iterable<Path>) walk.sorted(java.util.Comparator.reverseOrder())::iterator) Files.delete(path);
        }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Path root = Files.createTempDirectory(Path.of(System.getenv("ANDROID_DATA")), "identity-digest-");
        try { System.out.println("IdentityDigestContractTest all passed: " + contracts(root) + " checks"); }
        finally { deleteOwned(root); }
    }
}
