/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.keystore;

import android.security.keystore.KeyExpiredException;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyNotYetValidException;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;

import java.security.InvalidKeyException;

import java.security.InvalidAlgorithmParameterException;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.ProviderException;
import java.util.Arrays;
import java.util.Date;
import java.util.function.Consumer;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public final class KeySpecCases {
    private static final String PROVIDER = "AndroidKeyStore";
    private static final String AES = KeyProperties.KEY_ALGORITHM_AES;
    private static final String HMAC = KeyProperties.KEY_ALGORITHM_HMAC_SHA256;
    private static final int ENCRYPT_DECRYPT =
            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT;
    private static final String PREFIX = "key-spec-fixture-";
    private static final byte[] MESSAGE = {42, 13, 8, 63, 7};

    private KeySpecCases() {}

    public static void run(Consumer<String> report) throws Exception {
        KeyStore store = KeyStore.getInstance(PROVIDER);
        store.load(null);
        Checks checks = new Checks(report);
        try {
            checks.run("aes-gcm", () -> {
                SecretKey key = generate(AES, aes("gcm", KeyProperties.BLOCK_MODE_GCM).build());
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key);
                byte[] encrypted = cipher.doFinal(MESSAGE);
                var params = cipher.getParameters();
                Cipher decrypt = Cipher.getInstance("AES/GCM/NoPadding");
                decrypt.init(Cipher.DECRYPT_MODE, key, params);
                require(Arrays.equals(MESSAGE, decrypt.doFinal(encrypted)), "AES round trip");
            });
            checks.run("aes-randomized-ecb", () -> reject(AES,
                    aes("randomized-ecb", "eCb").build(), store));
            checks.run("aes-ecb-opt-out", () -> {
                SecretKey key = generate(AES, aes("ecb-opt-out", KeyProperties.BLOCK_MODE_ECB)
                        .setRandomizedEncryptionRequired(false).build());
                Cipher cipher = Cipher.getInstance("AES/ECB/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key);
                require(cipher.doFinal(new byte[16]).length == 16, "explicit ECB authorization");
            });
            checks.run("aes-decrypt-only-ecb", () -> generate(AES,
                    spec("decrypt-only-ecb", KeyProperties.PURPOSE_DECRYPT)
                            .setBlockModes(KeyProperties.BLOCK_MODE_ECB)
                            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build()));
            checks.run("aes-signature-padding", () -> reject(AES,
                    aes("signature-padding", KeyProperties.BLOCK_MODE_GCM)
                            .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PSS).build(),
                    store));
            checks.run("aes-unknown-block-mode", () -> reject(AES,
                    aes("unknown-mode", "unknown").build(), store));
            checks.run("aes-unknown-padding", () -> reject(AES,
                    aes("unknown-padding", KeyProperties.BLOCK_MODE_GCM)
                            .setEncryptionPaddings("unknown").build(), store));
            checks.run("aes-unknown-purpose", () -> reject(AES,
                    spec("unknown-purpose", 1 << 25)
                            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build(),
                    store));
            checks.run("aes-unknown-digest", () -> reject(AES,
                    aes("unknown-digest", KeyProperties.BLOCK_MODE_GCM)
                            .setDigests("unsupported").build(), store));
            checks.run("aes-zero-size", () -> reject(AES,
                    aes("zero", KeyProperties.BLOCK_MODE_GCM).setKeySize(0).build(), store));
            checks.run("hmac-default-digest", () -> require(mac(generate(HMAC,
                    spec("hmac-default", KeyProperties.PURPOSE_SIGN).build())).length == 32,
                    "implicit HmacSHA256 digest"));
            checks.run("hmac-matching-digest", () -> require(mac(generate(HMAC,
                    spec("hmac-match", KeyProperties.PURPOSE_SIGN).setDigests("sha-256")
                            .setKeySize(512).build())).length == 32, "explicit HmacSHA256 digest"));
            checks.run("hmac-mismatched-digest", () -> reject(HMAC,
                    spec("hmac-wrong", KeyProperties.PURPOSE_SIGN)
                            .setDigests(KeyProperties.DIGEST_SHA512).build(), store));
            checks.run("hmac-empty-digest", () -> reject(HMAC,
                    spec("hmac-empty", KeyProperties.PURPOSE_SIGN).setDigests().build(), store));
            checks.run("hmac-extra-digest", () -> reject(HMAC,
                    spec("hmac-extra", KeyProperties.PURPOSE_SIGN)
                            .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                            .build(), store));
            checks.run("hmac-oversize", () -> reject(HMAC,
                    spec("hmac-oversize", KeyProperties.PURPOSE_SIGN).setKeySize(520).build(), store));
            checks.run("failed-reinitialization", () -> {
                KeyGenerator generator = KeyGenerator.getInstance(HMAC, PROVIDER);
                KeyGenParameterSpec first = spec("existing", KeyProperties.PURPOSE_SIGN).build();
                generator.init(first);
                byte[] before = mac(generator.generateKey());
                expectInvalid(generator, spec("replacement", KeyProperties.PURPOSE_SIGN)
                        .setDigests(KeyProperties.DIGEST_SHA1).build());
                try {
                    generator.generateKey();
                    throw new AssertionError("reused the previous initialization");
                } catch (IllegalStateException expected) {
                    require(!store.containsAlias(PREFIX + "replacement"), "saved rejected key");
                    require(Arrays.equals(before, mac((SecretKey)
                            store.getKey(first.getKeystoreAlias(), null))), "replaced the prior key");
                }
            });
            checks.run("opaque-aes-and-key-info", () -> {
                SecretKey key = generate(AES, aes("info", KeyProperties.BLOCK_MODE_GCM)
                        .setKeySize(192).build());
                require(key.getEncoded() == null && key.getFormat() == null, "exportable key");
                SecretKey reloaded = (SecretKey) store.getKey(PREFIX + "info", null);
                require(reloaded.getEncoded() == null, "exportable retrieved key");
                KeyInfo info = keyInfo(reloaded);
                require(info.getKeystoreAlias().equals(PREFIX + "info")
                        && info.getOrigin() == KeyProperties.ORIGIN_GENERATED
                        && info.getKeySize() == 192
                        && info.getPurposes() == ENCRYPT_DECRYPT
                        && Arrays.equals(info.getBlockModes(), new String[] {"GCM"})
                        && Arrays.equals(info.getEncryptionPaddings(), new String[] {"NoPadding"})
                        && !info.isUserAuthenticationRequired(), "incorrect AES metadata");
            });
            checks.run("aes-purpose", () -> {
                SecretKey key = generate(AES, spec("decrypt-purpose", KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
                rejectKey(InvalidKeyException.class, () -> Cipher.getInstance("AES/GCM/NoPadding")
                        .init(Cipher.ENCRYPT_MODE, key));
            });
            checks.run("aes-block-mode", () -> {
                SecretKey key = generate(AES, aes("authorize-mode", "GCM").build());
                rejectKey(InvalidKeyException.class, () -> Cipher.getInstance("AES/CBC/NoPadding")
                        .init(Cipher.ENCRYPT_MODE, key));
            });
            checks.run("aes-padding", () -> {
                SecretKey key = generate(AES, aes("authorize-padding", "CBC").build());
                rejectKey(InvalidKeyException.class, () -> Cipher.getInstance("AES/CBC/PKCS7Padding")
                        .init(Cipher.ENCRYPT_MODE, key));
            });
            checks.run("aes-cbc-pkcs7", () -> {
                SecretKey key = generate(AES, spec("cbc-pkcs7", ENCRYPT_DECRYPT)
                        .setBlockModes("CBC").setEncryptionPaddings("PKCS7Padding").build());
                Cipher encrypt = Cipher.getInstance("AES/CBC/PKCS7Padding");
                encrypt.init(Cipher.ENCRYPT_MODE, key);
                byte[] encrypted = encrypt.doFinal(MESSAGE);
                Cipher decrypt = Cipher.getInstance("AES/CBC/PKCS7Padding");
                decrypt.init(Cipher.DECRYPT_MODE, key, encrypt.getParameters());
                require(Arrays.equals(MESSAGE, decrypt.doFinal(encrypted)), "CBC round trip");
            });
            checks.run("aes-iv-cannot-be-reused", () -> {
                SecretKey key = generate(AES, aes("cbc-iv-reuse", "CBC").build());
                Cipher cipher = Cipher.getInstance("AES/CBC/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key);
                byte[] firstIv = cipher.getIV();
                require(cipher.doFinal(new byte[16]).length == 16, "first encryption failed");
                try {
                    cipher.doFinal(new byte[16]);
                    throw new AssertionError("reused IV in an independent operation");
                } catch (IllegalStateException expected) {
                    // Encryption with the old IV is unavailable after finishing.
                }
                cipher.init(Cipher.ENCRYPT_MODE, key);
                require(!Arrays.equals(firstIv, cipher.getIV())
                        && cipher.doFinal(new byte[16]).length == 16, "failed to start a new IV");
            });
            checks.run("aes-caller-iv", () -> {
                SecretKey key = generate(AES, aes("authorize-iv", "GCM").build());
                try {
                    Cipher.getInstance("AES/GCM/NoPadding").init(Cipher.ENCRYPT_MODE, key,
                            new GCMParameterSpec(128, new byte[12]));
                    throw new AssertionError("accepted caller IV for randomized encryption");
                } catch (InvalidAlgorithmParameterException expected) {
                    // The parameters are valid GCM parameters, but forbidden by the key.
                }
            });
            checks.run("aes-caller-iv-opt-out", () -> {
                SecretKey key = generate(AES, aes("allow-iv", "GCM")
                        .setRandomizedEncryptionRequired(false).build());
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                byte[] iv = new byte[12];
                iv[11] = 42;
                cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(128, iv));
                require(Arrays.equals(iv, cipher.getIV())
                        && cipher.doFinal(MESSAGE).length == MESSAGE.length + 16, "wrong IV");
            });
            checks.run("aes-not-yet-valid", () -> {
                Date future = new Date(System.currentTimeMillis() + 86_400_000);
                SecretKey key = generate(AES, aes("future", "GCM").setKeyValidityStart(future)
                        .build());
                rejectKey(KeyNotYetValidException.class,
                        () -> Cipher.getInstance("AES/GCM/NoPadding").init(Cipher.ENCRYPT_MODE, key));
            });
            checks.run("aes-import-origination-end", () -> {
                Date past = new Date(System.currentTimeMillis() - 86_400_000);
                SecretKey raw = new SecretKeySpec(new byte[16], AES);
                Cipher encrypt = Cipher.getInstance("AES/GCM/NoPadding");
                encrypt.init(Cipher.ENCRYPT_MODE, raw);
                byte[] encrypted = encrypt.doFinal(MESSAGE);
                SecretKey key = importAes(store, "import-expired", raw,
                        new KeyProtection.Builder(ENCRYPT_DECRYPT).setBlockModes("GCM")
                                .setEncryptionPaddings("NoPadding")
                                .setKeyValidityForOriginationEnd(past).build());
                require(key.getEncoded() == null && keyInfo(key).getOrigin()
                        == KeyProperties.ORIGIN_IMPORTED, "wrong imported key metadata");
                require(keyInfo(key).getKeyValidityForOriginationEnd() != null
                        && keyInfo(key).getKeyValidityForConsumptionEnd() == null,
                        "mixed up origination and consumption");
                rejectKey(KeyExpiredException.class, () -> Cipher.getInstance("AES/GCM/NoPadding")
                        .init(Cipher.ENCRYPT_MODE, key));
                Cipher decrypt = Cipher.getInstance("AES/GCM/NoPadding");
                decrypt.init(Cipher.DECRYPT_MODE, key, encrypt.getParameters());
                require(Arrays.equals(MESSAGE, decrypt.doFinal(encrypted)), "imported key changed");
            });
            checks.run("aes-consumption-end", () -> {
                Date past = new Date(System.currentTimeMillis() - 86_400_000);
                SecretKey key = generate(AES, aes("consume-expired", "GCM")
                        .setKeyValidityForConsumptionEnd(past).build());
                Cipher encrypt = Cipher.getInstance("AES/GCM/NoPadding");
                encrypt.init(Cipher.ENCRYPT_MODE, key);
                require(encrypt.doFinal(MESSAGE).length == MESSAGE.length + 16, "origination denied");
                rejectKey(KeyExpiredException.class, () -> Cipher.getInstance("AES/GCM/NoPadding")
                        .init(Cipher.DECRYPT_MODE, key, encrypt.getParameters()));
            });
            checks.run("protected-import-required", () -> {
                String alias = PREFIX + "missing-protection";
                try {
                    store.setEntry(alias, new KeyStore.SecretKeyEntry(
                            new SecretKeySpec(new byte[16], AES)), null);
                    throw new AssertionError("imported key without authorizations");
                } catch (KeyStoreException expected) {
                    require(!store.containsAlias(alias), "stored an unauthorized import");
                }
            });
            checks.run("hmac-operation-digest-purpose", () -> {
                SecretKey key = generate(HMAC, spec("hmac-operation", KeyProperties.PURPOSE_SIGN)
                        .build());
                require(key.getEncoded() == null && Arrays.equals(keyInfo(key).getDigests(),
                        new String[] {KeyProperties.DIGEST_SHA256}), "wrong HMAC metadata");
                rejectKey(InvalidKeyException.class, () -> Mac.getInstance("HmacSHA512").init(key));
                SecretKey verify = generate(HMAC,
                        spec("hmac-verify-only", KeyProperties.PURPOSE_VERIFY).build());
                rejectKey(InvalidKeyException.class, () -> Mac.getInstance(HMAC).init(verify));
            });
            checks.run("hmac-rechecks-after-final", () -> {
                SecretKey key = generate(HMAC, spec("hmac-recheck", KeyProperties.PURPOSE_SIGN)
                        .build());
                Mac mac = Mac.getInstance(HMAC);
                mac.init(key);
                byte[] before = mac.doFinal(MESSAGE);
                require(Arrays.equals(before, mac.doFinal(MESSAGE)), "MAC did not reset");
                store.deleteEntry(PREFIX + "hmac-recheck");
                try {
                    mac.doFinal(MESSAGE);
                    throw new AssertionError("reused MAC after deleting its key");
                } catch (ProviderException expected) {
                    // A reused MAC must open a fresh authorized operation.
                }
            });
            checks.run("deleted-handle", () -> {
                SecretKey key = generate(AES, aes("deleted", "GCM").build());
                store.deleteEntry(PREFIX + "deleted");
                generate(AES, aes("deleted", "GCM").build());
                rejectKey(InvalidKeyException.class, () -> Cipher.getInstance("AES/GCM/NoPadding")
                        .init(Cipher.ENCRYPT_MODE, key));
            });
        } finally {
            for (String alias : java.util.Collections.list(store.aliases())) {
                if (alias.startsWith(PREFIX)) store.deleteEntry(alias);
            }
        }
        checks.finish();
    }

    private static KeyGenParameterSpec.Builder spec(String suffix, int purposes) {
        return new KeyGenParameterSpec.Builder(PREFIX + suffix, purposes);
    }

    private static KeyGenParameterSpec.Builder aes(String suffix, String mode) {
        return spec(suffix, ENCRYPT_DECRYPT).setBlockModes(mode)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE);
    }

    private static SecretKey generate(String algorithm, KeyGenParameterSpec spec) throws Exception {
        KeyGenerator generator = KeyGenerator.getInstance(algorithm, PROVIDER);
        generator.init(spec);
        return generator.generateKey();
    }

    private static void reject(String algorithm, KeyGenParameterSpec spec, KeyStore store)
            throws Exception {
        expectInvalid(KeyGenerator.getInstance(algorithm, PROVIDER), spec);
        require(!store.containsAlias(spec.getKeystoreAlias()), "stored a rejected key");
    }

    private static void expectInvalid(KeyGenerator generator, KeyGenParameterSpec spec)
            throws Exception {
        try {
            generator.init(spec);
            throw new AssertionError("accepted invalid spec");
        } catch (InvalidAlgorithmParameterException expected) {
            // The rejection belongs to initialization, before any key can be generated.
        }
    }

    private static byte[] mac(SecretKey key) throws Exception {
        Mac mac = Mac.getInstance(HMAC);
        mac.init(key);
        return mac.doFinal(MESSAGE);
    }

    private static SecretKey importAes(KeyStore store, String suffix, SecretKey key,
            KeyProtection protection) throws Exception {
        String alias = PREFIX + suffix;
        store.setEntry(alias, new KeyStore.SecretKeyEntry(key), protection);
        return ((KeyStore.SecretKeyEntry) store.getEntry(alias, null)).getSecretKey();
    }

    private static KeyInfo keyInfo(SecretKey key) throws Exception {
        return (KeyInfo) SecretKeyFactory.getInstance(key.getAlgorithm(), PROVIDER)
                .getKeySpec(key, KeyInfo.class);
    }

    private static void rejectKey(Class<? extends InvalidKeyException> type, Case test)
            throws Exception {
        try {
            test.run();
            throw new AssertionError("used a key outside its authorizations");
        } catch (InvalidKeyException expected) {
            if (!type.isInstance(expected)) throw expected;
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private interface Case {
        void run() throws Exception;
    }

    private static final class Checks {
        private final Consumer<String> report;
        private int total;
        private int passed;

        Checks(Consumer<String> report) {
            this.report = report;
        }

        void run(String name, Case test) {
            total++;
            try {
                test.run();
                passed++;
                report.accept("S04 " + name + " PASS");
            } catch (Exception | AssertionError e) {
                report.accept("S04 " + name + " FAIL " + e);
            }
        }

        void finish() {
            report.accept("S04 " + passed + "/" + total + " checks passed");
            require(passed == total, "Keystore spec fixture failed");
        }
    }
}
