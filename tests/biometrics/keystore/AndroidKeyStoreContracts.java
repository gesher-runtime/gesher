package com.gesher.tests;

import android.app.ActivityThread;
import android.app.Application;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;

import com.gesher.app.AndroidKeyStore;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.InvalidAlgorithmParameterException;
import java.security.Key;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.Provider;
import java.security.ProviderException;
import java.security.UnrecoverableKeyException;
import java.security.cert.Certificate;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

import javax.crypto.KeyGenerator;
import javax.crypto.spec.SecretKeySpec;

public final class AndroidKeyStoreContracts {
    private static final int GKS1 = 0x474b5331;
    private static final int GKS2 = 0x474b5332;
    private static final int ARCHIVED_LEGACY = 2;
    private static final String STORE = ".gesher-keystore";
    private static final Provider PROVIDER = new AndroidKeyStore();

    private record StoredKey(int kind, long created, String algorithm, byte[] key, byte[][] chain) {
        void write(DataOutputStream out, String alias) throws Exception {
            write(out, alias, -1);
        }

        void write(DataOutputStream out, String alias, int flag) throws Exception {
            out.writeUTF(alias);
            out.writeByte(kind);
            out.writeLong(created);
            out.writeUTF(algorithm);
            if (flag >= 0) out.writeByte(flag);
            out.writeInt(key.length);
            out.write(key);
            out.writeInt(chain.length);
            for (byte[] cert : chain) {
                out.writeInt(cert.length);
                out.write(cert);
            }
        }

        static StoredKey read(DataInputStream in, int kind, long created, String algorithm)
                throws Exception {
            byte[] key = in.readNBytes(in.readInt());
            byte[][] chain = new byte[in.readInt()][];
            for (int index = 0; index < chain.length; index++) {
                chain[index] = in.readNBytes(in.readInt());
            }
            return new StoredKey(kind, created, algorithm, key, chain);
        }

        boolean matches(StoredKey other) {
            return kind == other.kind && created == other.created && algorithm.equals(other.algorithm)
                    && Arrays.equals(key, other.key) && Arrays.deepEquals(chain, other.chain);
        }
    }

    private interface Operation {
        void run() throws Exception;
    }

    private static void rejected(Class<? extends Exception> type, Operation operation) throws Exception {
        try {
            operation.run();
        } catch (Exception failure) {
            if (type.isInstance(failure)) return;
            throw failure;
        }
        throw new AssertionError("Expected " + type.getName());
    }

    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError(detail);
    }

    private static KeyStore open() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore", PROVIDER);
        store.load(null);
        return store;
    }

    private static KeyGenParameterSpec spec(String alias, boolean requireAuthentication) {
        return new KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_SIGN
                | KeyProperties.PURPOSE_ENCRYPT)
                .setUserAuthenticationRequired(requireAuthentication)
                .build();
    }

    private static void seed(Path path) throws Exception {
        // Obtain an ordinary software certificate using the same provider, before the app exists.
        KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA", PROVIDER);
        generator.initialize(spec("seed", false));
        KeyPair pair = generator.generateKeyPair();
        byte[][] chain = {open().getCertificate("seed").getEncoded()};
        try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(path))) {
            out.writeInt(GKS1);
            out.writeInt(3);
            new StoredKey(1, 11, "AES", new byte[16], new byte[0][]).write(out, "old-secret");
            new StoredKey(2, 12, "RSA", pair.getPrivate().getEncoded(), chain).write(out, "old-private");
            new StoredKey(3, 13, "", new byte[0], chain).write(out, "old-certificate");
        }
    }

    private static Map<String, StoredKey> records(Path path, int format) throws Exception {
        Map<String, StoredKey> records = new HashMap<>();
        try (DataInputStream in = new DataInputStream(Files.newInputStream(path))) {
            check(in.readInt() == format, "store format");
            for (int remaining = in.readInt(); remaining > 0; remaining--) {
                String alias = in.readUTF();
                int kind = in.readByte();
                long created = in.readLong();
                String algorithm = in.readUTF();
                if (format == GKS2) {
                    check(in.readUnsignedByte() == (("old-secret".equals(alias) || "old-private".equals(alias))
                                    ? ARCHIVED_LEGACY : 0),
                            "unexpected legacy flag for " + alias);
                }
                records.put(alias, StoredKey.read(in, kind, created, algorithm));
            }
            check(in.read() == -1, "unexpected trailing records");
        }
        return records;
    }

    private static void rejectUnarchived(Path directory) throws Exception {
        Path active = directory.resolve(STORE);
        byte[] original = Files.readAllBytes(active);
        ActivityThread.application = new Application(directory.toFile());
        rejected(IOException.class, AndroidKeyStoreContracts::open);
        check(Arrays.equals(original, Files.readAllBytes(active)), "GKS1 must survive provider refusal");
        Map<String, StoredKey> entries = records(active, GKS1);
        for (int rawFlag : new int[] {1, ARCHIVED_LEGACY}) {
            try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(active))) {
                out.writeInt(GKS2);
                out.writeInt(entries.size());
                for (var entry : entries.entrySet()) {
                    StoredKey key = entry.getValue();
                    key.write(out, entry.getKey(), key.kind == 3 ? 0 : rawFlag);
                }
            }
            byte[] previousFormat = Files.readAllBytes(active);
            rejected(IOException.class, AndroidKeyStoreContracts::open);
            check(Arrays.equals(previousFormat, Files.readAllBytes(active)),
                    "raw GKS2 with flag " + rawFlag + " must survive provider refusal");
        }
    }

    // This JVM-only fixture covers the provider side of the sanitized protocol.
    // The native archive fixture exercises real file transactions and the Windows ACL.
    private static void seedScrubbedFixture(Path directory) throws Exception {
        Map<String, StoredKey> entries = records(directory.resolve("original.gks1"), GKS1);
        try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(directory.resolve(STORE)))) {
            out.writeInt(GKS2);
            out.writeInt(entries.size());
            for (var entry : entries.entrySet()) {
                StoredKey key = entry.getValue();
                StoredKey blocked = new StoredKey(key.kind, key.created, key.algorithm, new byte[0], key.chain);
                blocked.write(out, entry.getKey(), key.kind == 3 ? 0 : ARCHIVED_LEGACY);
            }
        }
    }

    private static void checkStore(Path directory, boolean reload) throws Exception {
        Path legacy = directory.resolve("original.gks1");
        Path active = directory.resolve(STORE);
        ActivityThread.application = new Application(directory.toFile());
        KeyStore store = open();

        for (String alias : new String[] {"old-secret", "old-private"}) {
            check(store.containsAlias(alias), alias + " must be preserved");
            rejected(UnrecoverableKeyException.class, () -> store.getKey(alias, null));
            rejected(UnrecoverableKeyException.class, () -> store.getEntry(alias, null));
        }
        Certificate trusted = store.getCertificate("old-certificate");
        check(trusted != null && trusted.equals(store.getCertificate("old-private")), "public certificates");

        if (!reload) {
            byte[] before = Files.readAllBytes(active);
            KeyGenerator secret = KeyGenerator.getInstance("AES", PROVIDER);
            KeyPairGenerator pair = KeyPairGenerator.getInstance("RSA", PROVIDER);
            rejected(InvalidAlgorithmParameterException.class, () -> secret.init(spec("rejected", true)));
            rejected(InvalidAlgorithmParameterException.class, () -> pair.initialize(spec("rejected", true)));
            KeyStore.SecretKeyEntry supplied = new KeyStore.SecretKeyEntry(new SecretKeySpec(new byte[16], "AES"));
            KeyProtection requiresAuth = new KeyProtection.Builder(KeyProperties.PURPOSE_ENCRYPT)
                    .setUserAuthenticationRequired(true).build();
            rejected(KeyStoreException.class, () -> store.setEntry("rejected", supplied, requiresAuth));
            check(!store.containsAlias("rejected"), "failed requests did not create a key");
            check(Arrays.equals(before, Files.readAllBytes(active)), "rejection must not rewrite the store");

            secret.init(spec("modern", false));
            Key key = secret.generateKey();
            check(Arrays.equals(key.getEncoded(), store.getKey("modern", null).getEncoded()),
                    "a new non-authentication key is usable");
        } else {
            check(store.getKey("modern", null) != null, "new key remains usable after restart");
        }

        Map<String, StoredKey> upgraded = records(active, GKS2);
        for (var entry : records(legacy, GKS1).entrySet()) {
            StoredKey old = entry.getValue();
            StoredKey expected = new StoredKey(old.kind, old.created, old.algorithm, new byte[0], old.chain);
            check(expected.matches(upgraded.get(entry.getKey())), "blocked metadata lost: " + entry.getKey());
        }
    }

    private static void checkFailedPersistence(Path directory) throws Exception {
        Path active = directory.resolve(STORE);
        byte[] original = Files.readAllBytes(active);
        ActivityThread.application = new Application(directory.toFile());
        KeyStore store = open();
        SecretKeySpec incoming = new SecretKeySpec(new byte[16], "AES");

        byte[] truncated = Arrays.copyOf(original, original.length - 1);
        Files.write(active, truncated);
        rejected(IOException.class, AndroidKeyStoreContracts::open);
        rejected(ProviderException.class, () -> store.containsAlias("old-secret"));
        rejected(KeyStoreException.class, () -> store.setKeyEntry("failed", incoming, null, null));
        rejected(KeyStoreException.class, () -> store.deleteEntry("old-certificate"));
        check(Arrays.equals(truncated, Files.readAllBytes(active)), "corrupt store must survive failed read");
        Files.write(active, original);
        check(store.containsAlias("old-certificate"), "valid file can be reloaded");

        // Occupy the exact destination during serialization, after the provider loaded the store
        // and created its staging file. The real atomic move cannot replace a nonempty directory.
        Path saved = directory.resolve("saved.gks2");
        Path blocker = active.resolve("blocker");
        SecretKeySpec failing = new SecretKeySpec(new byte[16], "AES") {
            private int encodings;

            @Override
            public byte[] getEncoded() {
                if (++encodings == 2) {
                    try {
                        Files.move(active, saved, StandardCopyOption.ATOMIC_MOVE);
                        Files.createDirectory(active);
                        Files.createFile(blocker);
                    } catch (IOException failure) {
                        throw new AssertionError("could not set up atomic-move failure", failure);
                    }
                }
                return super.getEncoded();
            }
        };
        rejected(KeyStoreException.class, () -> store.setKeyEntry("failed", failing, null, null));
        check(Arrays.equals(original, Files.readAllBytes(saved)), "previous payload must survive failed move");
        check(Files.exists(blocker), "failed move must not remove its existing destination");
        Files.delete(blocker);
        Files.delete(active);
        Files.move(saved, active, StandardCopyOption.ATOMIC_MOVE);
        check(!store.containsAlias("failed"), "uncommitted alias must not become visible");
        check(store.containsAlias("old-secret") && store.getCertificate("old-certificate") != null,
                "existing aliases survive rollback");
        try (var listing = Files.list(directory)) {
            check(listing.noneMatch(path -> path.getFileName().toString().endsWith(".tmp")),
                    "failed commit must clean its staging file");
        }
    }

    private static void await(Path marker) throws Exception {
        long started = System.nanoTime();
        while (!Files.exists(marker)) {
            if (System.nanoTime() - started >= 8_000_000_000L) {
                throw new AssertionError("timed out waiting for " + marker.getFileName());
            }
            Thread.sleep(10);
        }
    }

    private static SecretKeySpec key() { return new SecretKeySpec(new byte[16], "AES"); }

    private static void recursiveUpdate(Path directory) throws Exception {
        ActivityThread.application = new Application(directory.toFile());
        KeyStore store = open();
        store.setKeyEntry("outer", new SecretKeySpec(new byte[16], "AES") {
            private int encodings;

            @Override public byte[] getEncoded() {
                if (++encodings == 2) {
                    try {
                        rejected(KeyStoreException.class,
                                () -> store.setKeyEntry("recursive", key(), null, null));
                    } catch (Exception failure) {
                        throw new AssertionError("recursive update did not safely fail", failure);
                    }
                }
                return super.getEncoded();
            }
        }, null, null);
        check(store.containsAlias("outer") && !store.containsAlias("recursive"),
                "recursive callback cannot open another lock descriptor or lose the outer update");
    }

    private static void raceFirst(Path directory) throws Exception {
        ActivityThread.application = new Application(directory.toFile());
        KeyStore store = open();
        store.setKeyEntry("first", new SecretKeySpec(new byte[16], "AES") {
            private int encodings;

            @Override public byte[] getEncoded() {
                if (++encodings == 2) {
                    try {
                        Files.createFile(directory.resolve("first-in-write"));
                        await(directory.resolve("release-first"));
                    } catch (Exception failure) {
                        throw new AssertionError("cannot stage first writer", failure);
                    }
                }
                return super.getEncoded();
            }
        }, null, null);
    }

    private static void raceSecond(Path directory) throws Exception {
        open().setKeyEntry("pending", key(), null, null);
        ActivityThread.application = new Application(directory.toFile());
        await(directory.resolve("first-in-write"));
        Files.createFile(directory.resolve("second-attempt"));
        KeyStore store = open();  // Merge the entry created before Application existed.
        store.setKeyEntry("second", key(), null, null);
        store.deleteEntry("remove-target");
        Files.createFile(directory.resolve("second-finished"));
    }

    public static void main(String[] args) throws Exception {
        Path directory = Path.of(args[1]);
        switch (args[0]) {
            case "seed" -> {
                seed(directory.resolve(STORE));
                Files.copy(directory.resolve(STORE), directory.resolve("original.gks1"));
            }
            case "reject-unarchived" -> rejectUnarchived(directory);
            case "seed-scrubbed-fixture" -> seedScrubbedFixture(directory);
            case "failed-persistence" -> checkFailedPersistence(directory);
            case "migrate", "reload" -> checkStore(directory, "reload".equals(args[0]));
            case "recursive" -> recursiveUpdate(directory);
            case "race-seed" -> {
                ActivityThread.application = new Application(directory.toFile());
                open().setKeyEntry("remove-target", key(), null, null);
            }
            case "race-first" -> raceFirst(directory);
            case "race-second" -> raceSecond(directory);
            case "race-check" -> {
                ActivityThread.application = new Application(directory.toFile());
                KeyStore store = open();
                for (String alias : new String[] {"first", "second", "pending"}) {
                    check(store.getKey(alias, null) != null, "lost concurrent mutation: " + alias);
                }
                check(!store.containsAlias("remove-target"), "concurrent deletion was lost");
            }
            default -> throw new IllegalArgumentException(args[0]);
        }
        System.out.println("PASS AndroidKeyStore " + args[0]);
    }
}
