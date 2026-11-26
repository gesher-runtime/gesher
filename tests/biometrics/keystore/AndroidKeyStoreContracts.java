package com.gesher.tests;

import android.app.ActivityThread;
import android.app.Application;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;

import com.gesher.app.AndroidKeyStore;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.InvalidAlgorithmParameterException;
import java.security.Key;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.Provider;
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
    private static final String STORE = ".gesher-keystore";
    private static final Provider PROVIDER = new AndroidKeyStore();

    private record StoredKey(int kind, long created, String algorithm, byte[] key, byte[][] chain) {
        void write(DataOutputStream out, String alias) throws Exception {
            out.writeUTF(alias);
            out.writeByte(kind);
            out.writeLong(created);
            out.writeUTF(algorithm);
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
                    check(in.readBoolean() == ("old-secret".equals(alias) || "old-private".equals(alias)),
                            "unexpected legacy flag for " + alias);
                }
                records.put(alias, StoredKey.read(in, kind, created, algorithm));
            }
            check(in.read() == -1, "unexpected trailing records");
        }
        return records;
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
            check(Files.mismatch(active, legacy) == -1, "read must not rewrite GKS1");
            KeyGenerator secret = KeyGenerator.getInstance("AES", PROVIDER);
            KeyPairGenerator pair = KeyPairGenerator.getInstance("RSA", PROVIDER);
            rejected(InvalidAlgorithmParameterException.class, () -> secret.init(spec("rejected", true)));
            rejected(InvalidAlgorithmParameterException.class, () -> pair.initialize(spec("rejected", true)));
            KeyStore.SecretKeyEntry supplied = new KeyStore.SecretKeyEntry(new SecretKeySpec(new byte[16], "AES"));
            KeyProtection requiresAuth = new KeyProtection.Builder(KeyProperties.PURPOSE_ENCRYPT)
                    .setUserAuthenticationRequired(true).build();
            rejected(KeyStoreException.class, () -> store.setEntry("rejected", supplied, requiresAuth));
            check(!store.containsAlias("rejected"), "failed requests did not create a key");
            check(Files.mismatch(active, legacy) == -1, "rejection must not rewrite GKS1");

            secret.init(spec("modern", false));
            Key key = secret.generateKey();
            check(Arrays.equals(key.getEncoded(), store.getKey("modern", null).getEncoded()),
                    "a new non-authentication key is usable");
        } else {
            check(store.getKey("modern", null) != null, "new key remains usable after restart");
        }

        Map<String, StoredKey> upgraded = records(active, GKS2);
        for (var entry : records(legacy, GKS1).entrySet()) {
            check(entry.getValue().matches(upgraded.get(entry.getKey())), "record lost: " + entry.getKey());
        }
    }

    public static void main(String[] args) throws Exception {
        Path directory = Path.of(args[1]);
        if ("seed".equals(args[0])) {
            seed(directory.resolve(STORE));
            Files.copy(directory.resolve(STORE), directory.resolve("original.gks1"));
        } else {
            checkStore(directory, "reload".equals(args[0]));
        }
        System.out.println("PASS AndroidKeyStore " + args[0]);
    }
}
