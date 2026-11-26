/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.keystore;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.security.InvalidAlgorithmParameterException;
import java.security.KeyStore;
import java.util.Arrays;
import java.util.function.Consumer;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;

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
