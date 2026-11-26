/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.security.keystore.KeyExpiredException;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyNotYetValidException;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;
import android.security.keystore.UserAuthArgs;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.NotSerializableException;
import java.io.ObjectOutputStream;
import java.nio.ByteBuffer;
import java.security.AlgorithmParameters;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.Key;
import java.security.NoSuchAlgorithmException;
import java.security.Provider;
import java.security.ProviderException;
import java.security.SecureRandom;
import java.security.Security;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.InvalidKeySpecException;
import java.security.spec.KeySpec;
import java.util.Date;
import java.util.UUID;
import java.util.function.IntFunction;

import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.CipherSpi;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.Mac;
import javax.crypto.MacSpi;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactorySpi;
import javax.crypto.ShortBufferException;

import static android.security.keystore.KeyProperties.*;

/** In-process software operations for symmetric keys stored with an Android authorization. */
final class AndroidKeyStoreSecretKeys {
    private AndroidKeyStoreSecretKeys() {}

    private static final String[] HMAC_ALGORITHMS = {
        KEY_ALGORITHM_HMAC_SHA1, KEY_ALGORITHM_HMAC_SHA224, KEY_ALGORITHM_HMAC_SHA256,
        KEY_ALGORITHM_HMAC_SHA384, KEY_ALGORITHM_HMAC_SHA512
    };
    private static final String[] HMAC_DIGESTS = {
        DIGEST_SHA1, DIGEST_SHA224, DIGEST_SHA256, DIGEST_SHA384, DIGEST_SHA512
    };

    static void register(Provider provider) {
        service(provider, "Cipher", "AES/ECB/NoPadding", Ecb.class);
        service(provider, "Cipher", "AES/ECB/PKCS7Padding", EcbPkcs7.class);
        service(provider, "Cipher", "AES/CBC/NoPadding", Cbc.class);
        service(provider, "Cipher", "AES/CBC/PKCS7Padding", CbcPkcs7.class);
        service(provider, "Cipher", "AES/CTR/NoPadding", Ctr.class);
        service(provider, "Cipher", "AES/GCM/NoPadding", Gcm.class);
        service(provider, "SecretKeyFactory", KEY_ALGORITHM_AES, Factory.class);
        Class<?>[] macs = {Hmac1.class, Hmac224.class, Hmac256.class, Hmac384.class, Hmac512.class};
        for (int i = 0; i < HMAC_ALGORITHMS.length; i++) {
            service(provider, "Mac", HMAC_ALGORITHMS[i], macs[i]);
            service(provider, "SecretKeyFactory", HMAC_ALGORITHMS[i], Factory.class);
        }
    }

    private static void service(Provider provider, String kind, String algorithm, Class<?> spi) {
        String name = kind + "." + algorithm;
        provider.put(name, spi.getName());
        provider.put(name + " SupportedKeyClasses", StoredKey.class.getName());
    }

    private static String hmacDigest(String algorithm) throws InvalidAlgorithmParameterException {
        if (KEY_ALGORITHM_AES.equalsIgnoreCase(algorithm)) return null;
        for (int i = 0; i < HMAC_ALGORITHMS.length; i++) {
            if (HMAC_ALGORITHMS[i].equalsIgnoreCase(algorithm)) return HMAC_DIGESTS[i];
        }
        throw new InvalidAlgorithmParameterException("Unsupported secret key algorithm " + algorithm);
    }

    static String canonicalAlgorithm(String algorithm) throws InvalidAlgorithmParameterException {
        if (hmacDigest(algorithm) == null) return KEY_ALGORITHM_AES;
        for (String name : HMAC_ALGORITHMS) {
            if (name.equalsIgnoreCase(algorithm)) return name;
        }
        throw new AssertionError(algorithm);
    }

    static boolean validSize(String algorithm, int bits) {
        if (KEY_ALGORITHM_AES.equalsIgnoreCase(algorithm)) {
            return bits == 128 || bits == 192 || bits == 256;
        }
        return bits >= 64 && bits <= 512 && bits % 8 == 0;
    }

    private static void validateProtection(UserAuthArgs auth, boolean critical, int useCount)
            throws InvalidAlgorithmParameterException {
        if (auth.isUserAuthenticationRequired() || auth.isUserConfirmationRequired()
                || auth.isUserPresenceRequired() || auth.isUnlockedDeviceRequired()
                || auth.isUserAuthenticationValidWhileOnBody()
                || auth.getBoundToSpecificSecureUserId() != 0 || critical) {
            throw new InvalidAlgorithmParameterException(
                    "The Gesher software keystore cannot enforce this authentication policy");
        }
        if (useCount != UNRESTRICTED_USAGE_COUNT) {
            throw new InvalidAlgorithmParameterException(
                    "The Gesher software keystore cannot enforce a usage count");
        }
    }

    static Authorization generated(String algorithm, int bits, KeyGenParameterSpec spec)
            throws InvalidAlgorithmParameterException {
        if (spec.isStrongBoxBacked()) {
            throw new android.security.keystore.StrongBoxUnavailableException(
                    "Gesher has no StrongBox provider");
        }
        validateProtection(spec, spec.isCriticalToDeviceEncryption(), spec.getMaxUsageCount());
        if (spec.getNamespace() != NAMESPACE_APPLICATION || spec.getAttestationChallenge() != null
                || spec.getAttestKeyAlias() != null || spec.getAlgorithmParameterSpec() != null) {
            throw new InvalidAlgorithmParameterException(
                    "Unsupported namespace, attestation or algorithm parameters for a secret key");
        }
        return authorization(algorithm, bits, ORIGIN_GENERATED, spec.getPurposes(),
                spec.getBlockModes(), spec.getEncryptionPaddings(), spec.getSignaturePaddings(),
                spec.isDigestsSpecified() ? spec.getDigests() : null,
                spec.isRandomizedEncryptionRequired(), spec.getKeyValidityStart(),
                spec.getKeyValidityForOriginationEnd(), spec.getKeyValidityForConsumptionEnd());
    }

    static Authorization imported(String algorithm, int bits, KeyProtection spec)
            throws InvalidAlgorithmParameterException {
        validateProtection(spec, spec.isCriticalToDeviceEncryption(), spec.getMaxUsageCount());
        if (spec.isStrongBoxBacked() || spec.isRollbackResistant()) {
            throw new InvalidAlgorithmParameterException(
                    "The Gesher software keystore cannot enforce this hardware protection");
        }
        return authorization(algorithm, bits, ORIGIN_IMPORTED, spec.getPurposes(),
                spec.getBlockModes(), spec.getEncryptionPaddings(), spec.getSignaturePaddings(),
                spec.isDigestsSpecified() ? spec.getDigests() : null,
                spec.isRandomizedEncryptionRequired(), spec.getKeyValidityStart(),
                spec.getKeyValidityForOriginationEnd(), spec.getKeyValidityForConsumptionEnd())
                .forNewKey();
    }

    private static Authorization authorization(String algorithm, int bits, int origin, int purposes,
            String[] modes, String[] paddings, String[] signatures, String[] digests,
            boolean randomized, Date start, Date originationEnd, Date consumptionEnd)
            throws InvalidAlgorithmParameterException {
        try {
            KeyProperties.Purpose.allToKeymaster(purposes);
            KeyProperties.BlockMode.allToKeymaster(modes);
            KeyProperties.EncryptionPadding.allToKeymaster(paddings);
            if (signatures.length != 0) {
                throw new InvalidAlgorithmParameterException(
                        "Signature paddings not supported for symmetric keys");
            }
            if ((purposes & PURPOSE_ENCRYPT) != 0 && randomized && contains(modes, BLOCK_MODE_ECB)) {
                throw new InvalidAlgorithmParameterException(
                        "Randomized encryption required but ECB was specified");
            }
            String hmacDigest = hmacDigest(algorithm);
            if (digests != null) {
                int[] digestIds = KeyProperties.Digest.allToKeymaster(digests);
                if (hmacDigest != null && (digestIds.length != 1
                        || digestIds[0] != KeyProperties.Digest.toKeymaster(hmacDigest))) {
                    throw new InvalidAlgorithmParameterException(
                            "Only " + hmacDigest + " is supported for " + algorithm);
                }
            }
            String[] actualDigests = hmacDigest != null ? new String[] {hmacDigest}
                    : digests != null ? canonical(digests, KeyProperties.Digest::toKeymaster,
                            KeyProperties.Digest::fromKeymaster) : new String[0];
            return new Authorization(null, bits, origin, purposes,
                    canonical(modes, KeyProperties.BlockMode::toKeymaster,
                            KeyProperties.BlockMode::fromKeymaster),
                    canonical(paddings, KeyProperties.EncryptionPadding::toKeymaster,
                            KeyProperties.EncryptionPadding::fromKeymaster),
                    actualDigests, randomized, start, originationEnd, consumptionEnd);
        } catch (IllegalArgumentException e) {
            throw new InvalidAlgorithmParameterException(e);
        }
    }

    private static String[] canonical(String[] names, java.util.function.ToIntFunction<String> id,
            IntFunction<String> name) {
        String[] result = new String[names.length];
        for (int i = 0; i < result.length; i++) result[i] = name.apply(id.applyAsInt(names[i]));
        return result;
    }

    static final class Authorization {
        final UUID id;
        final int bits;
        final int origin;
        final int purposes;
        final String[] modes;
        final String[] paddings;
        final String[] digests;
        final boolean randomized;
        final Date start;
        final Date originationEnd;
        final Date consumptionEnd;

        Authorization(UUID id, int bits, int origin, int purposes, String[] modes,
                String[] paddings, String[] digests, boolean randomized, Date start,
                Date originationEnd, Date consumptionEnd) {
            this.id = id;
            this.bits = bits;
            this.origin = origin;
            this.purposes = purposes;
            this.modes = modes;
            this.paddings = paddings;
            this.digests = digests;
            this.randomized = randomized;
            this.start = start;
            this.originationEnd = originationEnd;
            this.consumptionEnd = consumptionEnd;
        }

        Authorization forNewKey() {
            return new Authorization(UUID.randomUUID(), bits, origin, purposes, modes, paddings,
                    digests, randomized, start, originationEnd, consumptionEnd);
        }

        void checkPurpose(int purpose) throws InvalidKeyException {
            if ((purposes & purpose) == 0) throw new InvalidKeyException("Unauthorized key purpose");
            Date now = new Date();
            if (start != null && now.before(start)) throw new KeyNotYetValidException();
            Date end = purpose == PURPOSE_ENCRYPT || purpose == PURPOSE_SIGN
                    ? originationEnd : consumptionEnd;
            if (end != null && now.after(end)) throw new KeyExpiredException();
        }

        KeyInfo keyInfo(String alias) {
            return new KeyInfo(alias, false, origin, bits, start, originationEnd, consumptionEnd,
                    purposes, paddings, new String[0], digests, modes, false, -1, 0,
                    false, false, false, false, false, false,
                    SECURITY_LEVEL_SOFTWARE, UNRESTRICTED_USAGE_COUNT);
        }

        void write(DataOutputStream out) throws IOException {
            out.writeLong(id.getMostSignificantBits());
            out.writeLong(id.getLeastSignificantBits());
            out.writeInt(bits);
            out.writeInt(origin);
            out.writeInt(purposes);
            writeNames(out, modes);
            writeNames(out, paddings);
            writeNames(out, digests);
            out.writeBoolean(randomized);
            writeDate(out, start);
            writeDate(out, originationEnd);
            writeDate(out, consumptionEnd);
        }

        static Authorization read(DataInputStream in) throws IOException {
            return new Authorization(new UUID(in.readLong(), in.readLong()), in.readInt(),
                    in.readInt(), in.readInt(), readNames(in), readNames(in), readNames(in),
                    in.readBoolean(), readDate(in), readDate(in), readDate(in));
        }

        private static void writeNames(DataOutputStream out, String[] names) throws IOException {
            out.writeInt(names.length);
            for (String name : names) out.writeUTF(name);
        }

        private static String[] readNames(DataInputStream in) throws IOException {
            int length = in.readInt();
            if (length < 0 || length > 1024) throw new IOException("Invalid authorization size");
            String[] names = new String[length];
            for (int i = 0; i < length; i++) names[i] = in.readUTF();
            return names;
        }

        private static void writeDate(DataOutputStream out, Date date) throws IOException {
            out.writeBoolean(date != null);
            if (date != null) out.writeLong(date.getTime());
        }

        private static Date readDate(DataInputStream in) throws IOException {
            return in.readBoolean() ? new Date(in.readLong()) : null;
        }
    }

    static final class StoredKey implements SecretKey {
        private static final long serialVersionUID = 1L;
        final String alias;
        final UUID id;
        private final String algorithm;

        StoredKey(String alias, UUID id, String algorithm) {
            this.alias = alias;
            this.id = id;
            this.algorithm = algorithm;
        }

        @Override public String getAlgorithm() { return algorithm; }
        @Override public String getFormat() { return null; }
        @Override public byte[] getEncoded() { return null; }

        private void writeObject(ObjectOutputStream out) throws IOException {
            throw new NotSerializableException("AndroidKeyStore key");
        }
    }

    private static boolean contains(String[] names, String wanted) {
        for (String name : names) {
            if (name.equalsIgnoreCase(wanted)) return true;
        }
        return false;
    }

    private static StoredKey storedKey(Key key, String algorithm) throws InvalidKeyException {
        if (!(key instanceof StoredKey stored) || !algorithm.equalsIgnoreCase(key.getAlgorithm())) {
            throw new InvalidKeyException("Expected an AndroidKeyStore " + algorithm + " key");
        }
        return stored;
    }

    private static boolean platformCrypto(Provider provider) {
        // Never hand an app-supplied provider the underlying encoding.
        return !AndroidKeyStore.NAME.equals(provider.getName())
                && provider.getClass().getClassLoader() == Cipher.class.getClassLoader();
    }

    public static final class Factory extends SecretKeyFactorySpi {
        @Override
        protected SecretKey engineGenerateSecret(KeySpec spec) throws InvalidKeySpecException {
            throw new InvalidKeySpecException("Use KeyGenerator or KeyStore.setEntry");
        }

        @Override
        protected KeySpec engineGetKeySpec(SecretKey key, Class<?> type)
                throws InvalidKeySpecException {
            if (type != KeyInfo.class || !(key instanceof StoredKey stored)) {
                throw new InvalidKeySpecException("Expected AndroidKeyStore key and KeyInfo");
            }
            try {
                return AndroidKeyStore.resolveSecret(stored).authorization.keyInfo(stored.alias);
            } catch (InvalidKeyException e) {
                throw new InvalidKeySpecException(e);
            }
        }

        @Override
        protected SecretKey engineTranslateKey(SecretKey key) throws InvalidKeyException {
            throw new InvalidKeyException("Use KeyStore.setEntry");
        }
    }

    public abstract static class AesCipher extends CipherSpi {
        private final String mode;
        private final String padding;
        private Cipher delegate;
        private StoredKey key;
        private int purpose;
        private boolean finished;

        AesCipher(String mode, String padding) {
            this.mode = mode;
            this.padding = padding;
        }

        @Override
        protected void engineSetMode(String name) throws NoSuchAlgorithmException {
            if (!mode.equalsIgnoreCase(name)) throw new NoSuchAlgorithmException(name);
        }

        @Override
        protected void engineSetPadding(String name) throws javax.crypto.NoSuchPaddingException {
            if (!padding.equalsIgnoreCase(name)) throw new javax.crypto.NoSuchPaddingException(name);
        }

        private Cipher newCipher() throws InvalidKeyException {
            for (Provider provider : Security.getProviders()) {
                if (!platformCrypto(provider)) continue;
                try {
                    return Cipher.getInstance("AES/" + mode + "/" + padding, provider);
                } catch (NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException e) {
                    if (ENCRYPTION_PADDING_PKCS7.equals(padding)) {
                        try {
                            // For AES, JCA's PKCS5Padding service applies the PKCS7 block rule.
                            return Cipher.getInstance("AES/" + mode + "/PKCS5Padding", provider);
                        } catch (NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException ignored) {
                            // Try the next platform provider.
                        }
                    }
                }
            }
            throw new InvalidKeyException("No platform AES cipher for " + mode + "/" + padding);
        }

        private Key authorize(int opmode, Key key, boolean hasParams)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            delegate = null;
            this.key = storedKey(key, KEY_ALGORITHM_AES);
            int purpose;
            switch (opmode) {
                case Cipher.ENCRYPT_MODE:
                case Cipher.WRAP_MODE:
                    purpose = PURPOSE_ENCRYPT;
                    break;
                case Cipher.DECRYPT_MODE:
                case Cipher.UNWRAP_MODE:
                    purpose = PURPOSE_DECRYPT;
                    break;
                default:
                    throw new InvalidKeyException("Unsupported cipher mode");
            }
            var entry = AndroidKeyStore.resolveSecret(this.key);
            var policy = entry.authorization;
            policy.checkPurpose(purpose);
            if (!contains(policy.modes, mode) || !contains(policy.paddings, padding)) {
                throw new InvalidKeyException("Unauthorized AES block mode or padding");
            }
            if (purpose == PURPOSE_ENCRYPT && policy.randomized && hasParams) {
                throw new InvalidAlgorithmParameterException("Caller-provided IV is not permitted");
            }
            this.purpose = purpose;
            finished = false;
            return entry.key;
        }

        private Cipher operation() {
            if (finished) {
                if (purpose == PURPOSE_ENCRYPT && !BLOCK_MODE_ECB.equals(mode)) {
                    throw new IllegalStateException("IV already used; reinitialize the cipher");
                }
                try {
                    AndroidKeyStore.resolveSecret(key).authorization.checkPurpose(purpose);
                } catch (InvalidKeyException e) {
                    throw new ProviderException(e);
                }
                finished = false;
            }
            return delegate;
        }

        @Override
        protected void engineInit(int opmode, Key key, SecureRandom random) throws InvalidKeyException {
            try {
                engineInit(opmode, key, (AlgorithmParameterSpec) null, random);
            } catch (InvalidAlgorithmParameterException e) {
                throw new InvalidKeyException(e);
            }
        }

        @Override
        protected void engineInit(int opmode, Key key, AlgorithmParameterSpec params,
                SecureRandom random) throws InvalidKeyException, InvalidAlgorithmParameterException {
            Key raw = authorize(opmode, key, params != null);
            Cipher cipher = newCipher();
            cipher.init(opmode, raw, params, random);
            delegate = cipher;
        }

        @Override
        protected void engineInit(int opmode, Key key, AlgorithmParameters params,
                SecureRandom random) throws InvalidKeyException, InvalidAlgorithmParameterException {
            Key raw = authorize(opmode, key, params != null);
            Cipher cipher = newCipher();
            cipher.init(opmode, raw, params, random);
            delegate = cipher;
        }

        @Override protected int engineGetBlockSize() { return 16; }
        @Override protected byte[] engineGetIV() { return delegate == null ? null : delegate.getIV(); }
        @Override protected AlgorithmParameters engineGetParameters() {
            return delegate == null ? null : delegate.getParameters();
        }

        @Override protected int engineGetOutputSize(int len) { return delegate.getOutputSize(len); }

        @Override
        protected byte[] engineUpdate(byte[] input, int offset, int len) {
            return operation().update(input, offset, len);
        }

        @Override
        protected int engineUpdate(byte[] input, int offset, int len, byte[] output, int outOffset)
                throws ShortBufferException {
            return operation().update(input, offset, len, output, outOffset);
        }

        @Override
        protected int engineUpdate(ByteBuffer input, ByteBuffer output) throws ShortBufferException {
            return operation().update(input, output);
        }

        @Override
        protected void engineUpdateAAD(byte[] input, int offset, int len) {
            operation().updateAAD(input, offset, len);
        }

        @Override
        protected void engineUpdateAAD(ByteBuffer input) { operation().updateAAD(input); }

        @Override
        protected byte[] engineDoFinal(byte[] input, int offset, int len)
                throws IllegalBlockSizeException, BadPaddingException {
            Cipher cipher = operation();
            byte[] result = input == null ? cipher.doFinal() : cipher.doFinal(input, offset, len);
            finished = true;
            return result;
        }

        @Override
        protected int engineDoFinal(byte[] input, int offset, int len, byte[] output, int outOffset)
                throws ShortBufferException, IllegalBlockSizeException, BadPaddingException {
            Cipher cipher = operation();
            int written = input == null ? cipher.doFinal(output, outOffset)
                    : cipher.doFinal(input, offset, len, output, outOffset);
            finished = true;
            return written;
        }

        @Override
        protected int engineDoFinal(ByteBuffer input, ByteBuffer output)
                throws ShortBufferException, IllegalBlockSizeException, BadPaddingException {
            int written = operation().doFinal(input, output);
            finished = true;
            return written;
        }

        @Override
        protected byte[] engineWrap(Key key) throws IllegalBlockSizeException, InvalidKeyException {
            byte[] result = operation().wrap(key);
            finished = true;
            return result;
        }

        @Override
        protected Key engineUnwrap(byte[] wrapped, String algorithm, int type)
                throws InvalidKeyException, NoSuchAlgorithmException {
            Key result = operation().unwrap(wrapped, algorithm, type);
            finished = true;
            return result;
        }
    }

    public static final class Ecb extends AesCipher {
        public Ecb() { super(BLOCK_MODE_ECB, ENCRYPTION_PADDING_NONE); }
    }

    public static final class EcbPkcs7 extends AesCipher {
        public EcbPkcs7() { super(BLOCK_MODE_ECB, ENCRYPTION_PADDING_PKCS7); }
    }

    public static final class Cbc extends AesCipher {
        public Cbc() { super(BLOCK_MODE_CBC, ENCRYPTION_PADDING_NONE); }
    }

    public static final class CbcPkcs7 extends AesCipher {
        public CbcPkcs7() { super(BLOCK_MODE_CBC, ENCRYPTION_PADDING_PKCS7); }
    }

    public static final class Ctr extends AesCipher {
        public Ctr() { super(BLOCK_MODE_CTR, ENCRYPTION_PADDING_NONE); }
    }

    public static final class Gcm extends AesCipher {
        public Gcm() { super(BLOCK_MODE_GCM, ENCRYPTION_PADDING_NONE); }
    }

    public abstract static class Hmac extends MacSpi {
        private final String algorithm;
        private final String digest;
        private final int length;
        private Mac delegate;
        private StoredKey key;
        private boolean reset;

        Hmac(String algorithm, String digest, int length) {
            this.algorithm = algorithm;
            this.digest = digest;
            this.length = length;
        }

        @Override
        protected void engineInit(Key key, AlgorithmParameterSpec params)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            delegate = null;
            StoredKey stored = storedKey(key, algorithm);
            var entry = AndroidKeyStore.resolveSecret(stored);
            entry.authorization.checkPurpose(PURPOSE_SIGN);
            if (!contains(entry.authorization.digests, digest)) {
                throw new InvalidKeyException("Unauthorized HMAC digest");
            }
            if (params != null) throw new InvalidAlgorithmParameterException("HMAC has no parameters");
            for (Provider provider : Security.getProviders()) {
                if (!platformCrypto(provider)) continue;
                try {
                    Mac mac = Mac.getInstance(algorithm, provider);
                    mac.init(entry.key);
                    delegate = mac;
                    this.key = stored;
                    reset = false;
                    return;
                } catch (NoSuchAlgorithmException e) {
                    // Try the next platform provider.
                }
            }
            throw new InvalidKeyException("No platform MAC for " + algorithm);
        }

        private Mac operation() {
            if (reset) {
                try {
                    AndroidKeyStore.resolveSecret(key).authorization.checkPurpose(PURPOSE_SIGN);
                } catch (InvalidKeyException e) {
                    throw new ProviderException(e);
                }
                reset = false;
            }
            return delegate;
        }

        @Override protected int engineGetMacLength() { return length; }
        @Override protected void engineUpdate(byte input) { operation().update(input); }
        @Override protected void engineUpdate(byte[] input, int offset, int len) {
            operation().update(input, offset, len);
        }
        @Override protected void engineUpdate(ByteBuffer input) { operation().update(input); }
        @Override
        protected byte[] engineDoFinal() {
            byte[] result = operation().doFinal();
            reset = true;
            return result;
        }
        @Override
        protected void engineReset() {
            if (delegate != null) {
                delegate.reset();
                reset = true;
            }
        }
    }

    public static final class Hmac1 extends Hmac {
        public Hmac1() { super(KEY_ALGORITHM_HMAC_SHA1, DIGEST_SHA1, 20); }
    }

    public static final class Hmac224 extends Hmac {
        public Hmac224() { super(KEY_ALGORITHM_HMAC_SHA224, DIGEST_SHA224, 28); }
    }

    public static final class Hmac256 extends Hmac {
        public Hmac256() { super(KEY_ALGORITHM_HMAC_SHA256, DIGEST_SHA256, 32); }
    }

    public static final class Hmac384 extends Hmac {
        public Hmac384() { super(KEY_ALGORITHM_HMAC_SHA384, DIGEST_SHA384, 48); }
    }

    public static final class Hmac512 extends Hmac {
        public Hmac512() { super(KEY_ALGORITHM_HMAC_SHA512, DIGEST_SHA512, 64); }
    }
}
