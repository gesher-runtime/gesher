/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyStoreCryptoOperation;
import android.security.keystore.UserNotAuthenticatedException;

import com.gesher.server.KeystoreAuthorization;
import com.gesher.server.KeystoreAuthorizationService;

import java.math.BigInteger;
import java.security.AlgorithmParameters;
import java.security.GeneralSecurityException;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.InvalidParameterException;
import java.security.Key;
import java.security.KeyFactory;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.ProviderException;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.SignatureException;
import java.security.SignatureSpi;
import java.security.interfaces.ECKey;
import java.security.interfaces.RSAKey;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.InvalidParameterSpecException;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Arrays;
import java.util.Collections;
import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.CipherSpi;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.Mac;
import javax.crypto.MacSpi;
import javax.crypto.NoSuchPaddingException;
import javax.crypto.SecretKey;
import javax.crypto.ShortBufferException;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * AndroidKeyStore keys that require user authentication (ADR 0095): their key objects and
 * the Cipher, Signature and Mac that use them.
 *
 * <p>Such a key is in the app's key store only as a blob sealed by the system server's
 * {@code gesher.keystore} ({@link KeystoreAuthorization}), which gives the key material
 * back under Android's rules: within the key's timeout after the user confirmed with
 * Windows Hello, or to one operation that the user authorized through
 * {@code BiometricPrompt.CryptoObject}. The key objects are opaque (no format, no
 * encoding), as android.security.keystore2.AndroidKeyStoreKey, so other providers decline
 * them and JCA selects the primitives here, which are
 * {@link KeyStoreCryptoOperation}s: {@code CryptoObject.getOpId()} finds their handle.
 * Once the server has released the key, another provider's primitive does the work in
 * this process. Keys without the requirement never come here.
 */
final class AndroidKeyStoreAuth {
    private AndroidKeyStoreAuth() {
    }

    /** KeymasterDefs.KM_ERROR_KEY_USER_NOT_AUTHENTICATED. */
    private static final int KEY_USER_NOT_AUTHENTICATED = -26;

    // --- what a spec asks -----------------------------------------------------------------

    /**
     * KeyStore2ParameterUtils.addSids for a key that requires user authentication:
     * the device credential needs a secure lock screen (here: Windows Hello), and a key
     * that only a biometric may authorize needs an enrolled biometric, which Gesher never
     * has (Windows Hello does not say that a biometric was used).
     *
     * @throws IllegalStateException with AOSP's messages
     */
    static void check(int authType) {
        int both = KeyProperties.AUTH_BIOMETRIC_STRONG | KeyProperties.AUTH_DEVICE_CREDENTIAL;
        if (authType != both) {
            if ((authType & KeyProperties.AUTH_BIOMETRIC_STRONG) != 0) {
                throw new IllegalStateException("At least one biometric must be enrolled to"
                        + " create keys requiring user authentication for every use");
            } else if ((authType & KeyProperties.AUTH_DEVICE_CREDENTIAL) == 0) {
                throw new IllegalStateException("Invalid or no authentication type specified.");
            }
        }
        KeystoreAuthorizationService.Authorizer authorizer = authorizer();
        if (authorizer == null || authorizer.availability() != KeystoreAuthorization.OK) {
            throw new IllegalStateException("Secure lock screen must be enabled"
                    + " to create keys requiring user authentication");
        }
    }

    /** Seals new key material; the blob to store. */
    static byte[] seal(String alias, byte[] material, int timeoutSeconds, int authType)
            throws GeneralSecurityException {
        KeystoreAuthorizationService.Authorizer authorizer = authorizer();
        KeystoreAuthorization.Reply reply = authorizer != null
                ? authorizer.seal(material, Math.max(timeoutSeconds, 0), authType)
                : null;
        if (reply == null || reply.status != KeystoreAuthorization.OK || reply.data == null) {
            throw new GeneralSecurityException("The key " + alias + " requires user"
                    + " authentication and could not be bound to Windows Hello ("
                    + statusName(reply != null ? reply.status : KeystoreAuthorization.UNAVAILABLE)
                    + ")");
        }
        return reply.data;
    }

    private static String statusName(int status) {
        switch (status) {
            case KeystoreAuthorization.UNAVAILABLE: return "Windows Hello is not available";
            case KeystoreAuthorization.CANCELED: return "the user canceled Windows Hello";
            case KeystoreAuthorization.NOT_FOREGROUND: return "the app is not in the foreground";
            default: return "status " + status;
        }
    }

    private static KeystoreAuthorizationService.Authorizer sAuthorizer;

    /** The system server's keystore authorization, or null on a device without it. */
    private static synchronized KeystoreAuthorizationService.Authorizer authorizer() {
        if (sAuthorizer == null) sAuthorizer = KeystoreAuthorizationService.Client.get();
        return sAuthorizer;
    }

    // --- keys -----------------------------------------------------------------------------

    /** A key whose material is sealed: only its alias and algorithm are here. */
    abstract static class AuthKey implements Key {
        private static final long serialVersionUID = 1L;
        final String alias;
        final String algorithm;

        AuthKey(String alias, String algorithm) {
            this.alias = alias;
            this.algorithm = algorithm;
        }

        @Override
        public String getAlgorithm() {
            return algorithm;
        }

        @Override
        public String getFormat() {
            return null;
        }

        @Override
        public byte[] getEncoded() {
            return null;
        }

        @Override
        public boolean equals(Object o) {
            return o != null && o.getClass() == getClass() && ((AuthKey) o).alias.equals(alias)
                    && ((AuthKey) o).algorithm.equals(algorithm);
        }

        @Override
        public int hashCode() {
            return alias.hashCode() * 31 + algorithm.hashCode();
        }
    }

    static final class AuthSecretKey extends AuthKey implements SecretKey {
        private static final long serialVersionUID = 1L;

        AuthSecretKey(String alias, String algorithm) {
            super(alias, algorithm);
        }
    }

    /** As AndroidKeyStoreRSAPrivateKey: an RSAKey, not an RSAPrivateKey. */
    static final class AuthRsaPrivateKey extends AuthKey implements PrivateKey, RSAKey {
        private static final long serialVersionUID = 1L;
        private final BigInteger modulus;

        AuthRsaPrivateKey(String alias, BigInteger modulus) {
            super(alias, "RSA");
            this.modulus = modulus;
        }

        @Override
        public BigInteger getModulus() {
            return modulus;
        }
    }

    /** As AndroidKeyStoreECPrivateKey: an ECKey, not an ECPrivateKey. */
    static final class AuthEcPrivateKey extends AuthKey implements PrivateKey, ECKey {
        private static final long serialVersionUID = 1L;
        private final transient ECParameterSpec params;

        AuthEcPrivateKey(String alias, ECParameterSpec params) {
            super(alias, "EC");
            this.params = params;
        }

        @Override
        public ECParameterSpec getParams() {
            return params;
        }
    }

    /** The private key object of a sealed entry, from its certificate's public key. */
    static PrivateKey privateKey(String alias, PublicKey publicKey) {
        if (publicKey instanceof RSAKey) {
            return new AuthRsaPrivateKey(alias, ((RSAKey) publicKey).getModulus());
        }
        if (publicKey instanceof ECKey) {
            return new AuthEcPrivateKey(alias, ((ECKey) publicKey).getParams());
        }
        throw new ProviderException("Unsupported key algorithm: " + publicKey.getAlgorithm());
    }

    // --- one key's authorization for one primitive ----------------------------------------

    /**
     * The key of a primitive: released at init for a key with a timeout, or begun at init
     * and released at first use for a key that every use must authorize.
     */
    private static final class Gate {
        private AndroidKeyStore.Entry mEntry;
        private boolean mKind;   // true: secret key
        private long mHandle;    // the begun operation, 0 when none
        private boolean mUsed;   // a per-use operation was finished or failed

        /** The usable key now (a timeout), or null (every use: {@link #take} later). */
        Key init(Key key) throws InvalidKeyException {
            abort();
            mUsed = false;
            if (!(key instanceof AuthKey)) {
                throw new InvalidKeyException("Unsupported key: "
                        + (key != null ? key.getClass().getName() : "null"));
            }
            AndroidKeyStore.Entry entry = AndroidKeyStore.entry(((AuthKey) key).alias);
            if (entry == null || entry.sealed == null
                    || !entry.algorithm.equalsIgnoreCase(key.getAlgorithm())) {
                // keystore2's KEY_NOT_FOUND, as KeyStoreCryptoOperationUtils maps it.
                throw new KeyPermanentlyInvalidatedException();
            }
            mEntry = entry;
            mKind = key instanceof SecretKey;
            if (entry.authTimeout > 0) return release();
            KeystoreAuthorizationService.Authorizer authorizer = authorizer();
            KeystoreAuthorization.Reply reply = authorizer != null
                    ? authorizer.begin(entry.sealed) : null;
            if (reply != null && reply.status == KeystoreAuthorization.OK && reply.handle != 0) {
                mHandle = reply.handle;
                return null;
            }
            throw failure(reply);
        }

        boolean perUse() {
            return mEntry != null && mEntry.authTimeout <= 0;
        }

        long handle() {
            return mHandle;
        }

        /** A key with a timeout, if the user confirmed recently enough. */
        Key release() throws InvalidKeyException {
            KeystoreAuthorizationService.Authorizer authorizer = authorizer();
            KeystoreAuthorization.Reply reply = authorizer != null
                    ? authorizer.release(mEntry.sealed) : null;
            if (reply != null && reply.status == KeystoreAuthorization.OK && reply.data != null) {
                return key(reply.data);
            }
            throw failure(reply);
        }

        /** The key of the begun operation, once it was authorized. */
        Key take() throws android.security.KeyStoreException, InvalidKeyException {
            long handle = mHandle;
            mHandle = 0;
            if (handle == 0 || mUsed) throw notAuthenticated();
            mUsed = true;
            KeystoreAuthorizationService.Authorizer authorizer = authorizer();
            KeystoreAuthorization.Reply reply = authorizer != null
                    ? authorizer.finish(handle) : null;
            if (reply != null && reply.status == KeystoreAuthorization.OK && reply.data != null) {
                return key(reply.data);
            }
            throw notAuthenticated();
        }

        void abort() {
            long handle = mHandle;
            mHandle = 0;
            if (handle == 0) return;
            KeystoreAuthorizationService.Authorizer authorizer = authorizer();
            if (authorizer != null) authorizer.abort(handle);
        }

        private Key key(byte[] material) throws InvalidKeyException {
            try {
                if (mKind) return new SecretKeySpec(material, mEntry.algorithm);
                return KeyFactory.getInstance(mEntry.algorithm)
                        .generatePrivate(new PKCS8EncodedKeySpec(material));
            } catch (GeneralSecurityException | RuntimeException e) {
                throw new InvalidKeyException("The released key is not usable", e);
            } finally {
                Arrays.fill(material, (byte) 0);
            }
        }

        /** KeyStoreCryptoOperationUtils.getInvalidKeyException. */
        private static InvalidKeyException failure(KeystoreAuthorization.Reply reply) {
            int status = reply != null ? reply.status : KeystoreAuthorization.UNAVAILABLE;
            switch (status) {
                case KeystoreAuthorization.NOT_AUTHENTICATED:
                    return new UserNotAuthenticatedException();
                case KeystoreAuthorization.INVALIDATED:
                    return new KeyPermanentlyInvalidatedException();
                default:
                    return new InvalidKeyException("Keystore operation failed: "
                            + statusName(status));
            }
        }

        static android.security.KeyStoreException notAuthenticated() {
            return new android.security.KeyStoreException(KEY_USER_NOT_AUTHENTICATED,
                    "Key user not authenticated");
        }
    }

    // --- the provider's services ----------------------------------------------------------

    /** A service for sealed keys only, as AndroidKeyStoreBCWorkaroundProvider's. */
    private static final class Service extends Provider.Service {
        Service(Provider provider, String type, String algorithm, Class<?> implementation) {
            super(provider, type, algorithm, implementation.getName(), null,
                    Collections.singletonMap("SupportedKeyClasses", AuthKey.class.getName()));
        }

        @Override
        public boolean supportsParameter(Object parameter) {
            return parameter instanceof AuthKey;
        }

        @Override
        public Object newInstance(Object constructorParameter) {
            switch (getType()) {
                case "Cipher": return new AuthCipher(getAlgorithm());
                case "Signature": return new AuthSignature(getAlgorithm());
                default: return new AuthMac(getAlgorithm());
            }
        }
    }

    /** The transformations and algorithms AndroidKeyStoreBCWorkaroundProvider registers. */
    static void register(AndroidKeyStore provider, Registrar registrar) {
        for (String t : new String[] {"AES/ECB/NoPadding", "AES/ECB/PKCS7Padding",
                "AES/CBC/NoPadding", "AES/CBC/PKCS7Padding", "AES/CTR/NoPadding",
                "AES/GCM/NoPadding", "RSA/ECB/NoPadding", "RSA/ECB/PKCS1Padding",
                "RSA/ECB/OAEPPadding", "RSA/ECB/OAEPWithSHA-1AndMGF1Padding",
                "RSA/ECB/OAEPWithSHA-224AndMGF1Padding", "RSA/ECB/OAEPWithSHA-256AndMGF1Padding",
                "RSA/ECB/OAEPWithSHA-384AndMGF1Padding",
                "RSA/ECB/OAEPWithSHA-512AndMGF1Padding"}) {
            registrar.add(new Service(provider, "Cipher", t, AuthCipher.class));
        }
        for (String digest : new String[] {"SHA1", "SHA224", "SHA256", "SHA384", "SHA512"}) {
            registrar.add(new Service(provider, "Mac", "Hmac" + digest, AuthMac.class));
            registrar.add(new Service(provider, "Signature", digest + "withRSA",
                    AuthSignature.class));
            registrar.add(new Service(provider, "Signature", digest + "withRSA/PSS",
                    AuthSignature.class));
            registrar.add(new Service(provider, "Signature", digest + "withECDSA",
                    AuthSignature.class));
        }
        registrar.add(new Service(provider, "Signature", "MD5withRSA", AuthSignature.class));
        registrar.add(new Service(provider, "Signature", "NONEwithRSA", AuthSignature.class));
        registrar.add(new Service(provider, "Signature", "NONEwithECDSA", AuthSignature.class));
    }

    /** Provider.putService is protected: the provider passes it in. */
    interface Registrar {
        void add(Provider.Service service);
    }

    // --- Cipher ---------------------------------------------------------------------------

    /** A Cipher over a sealed key: AndroidKeyStoreCipherSpiBase's behavior. */
    public static final class AuthCipher extends CipherSpi implements KeyStoreCryptoOperation {
        private final String mTransformation;
        private final Gate mGate = new Gate();
        private Cipher mDelegate;          // initialized with the released key, or null
        private int mOpmode;
        private AlgorithmParameterSpec mSpec;
        private SecureRandom mRandom;
        private boolean mInitialized;
        private Exception mCachedException;

        AuthCipher(String transformation) {
            mTransformation = transformation;
        }

        private boolean aes() {
            return mTransformation.startsWith("AES/");
        }

        private boolean gcm() {
            return mTransformation.startsWith("AES/GCM/");
        }

        private boolean needsIv() {
            return aes() && !mTransformation.startsWith("AES/ECB/");
        }

        @Override
        public long getOperationHandle() {
            return mGate.handle();
        }

        @Override
        protected void engineSetMode(String mode) throws NoSuchAlgorithmException {
            throw new NoSuchAlgorithmException("Unsupported mode: " + mode);
        }

        @Override
        protected void engineSetPadding(String padding) throws NoSuchPaddingException {
            throw new NoSuchPaddingException("Unsupported padding: " + padding);
        }

        @Override
        protected void engineInit(int opmode, Key key, SecureRandom random)
                throws InvalidKeyException {
            try {
                init(opmode, key, null, random);
            } catch (InvalidAlgorithmParameterException e) {
                throw new InvalidKeyException(e);
            }
        }

        @Override
        protected void engineInit(int opmode, Key key, AlgorithmParameterSpec params,
                SecureRandom random) throws InvalidKeyException,
                InvalidAlgorithmParameterException {
            init(opmode, key, params, random);
        }

        @Override
        protected void engineInit(int opmode, Key key, AlgorithmParameters params,
                SecureRandom random) throws InvalidKeyException,
                InvalidAlgorithmParameterException {
            AlgorithmParameterSpec spec = null;
            if (params != null) {
                try {
                    spec = gcm() ? params.getParameterSpec(GCMParameterSpec.class)
                            : aes() ? params.getParameterSpec(IvParameterSpec.class)
                            : params.getParameterSpec(AlgorithmParameterSpec.class);
                } catch (InvalidParameterSpecException e) {
                    throw new InvalidAlgorithmParameterException(e);
                }
            }
            init(opmode, key, spec, random);
        }

        private void init(int opmode, Key key, AlgorithmParameterSpec spec, SecureRandom random)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            mInitialized = false;
            mDelegate = null;
            mCachedException = null;
            boolean encrypt = opmode == Cipher.ENCRYPT_MODE || opmode == Cipher.WRAP_MODE;
            if (key instanceof PrivateKey && encrypt) {
                throw new InvalidKeyException("Encryption with a private key is not supported");
            }
            if (needsIv() && spec == null) {
                if (!encrypt) {
                    throw new InvalidAlgorithmParameterException(
                            "IV required when decrypting. Use IvParameterSpec or"
                            + " AlgorithmParameters to provide it.");
                }
                // The IV is the caller's to read after init, before the key is released.
                byte[] iv = new byte[gcm() ? 12 : 16];
                (random != null ? random : new SecureRandom()).nextBytes(iv);
                spec = gcm() ? new GCMParameterSpec(128, iv) : new IvParameterSpec(iv);
            }
            mOpmode = opmode;
            mSpec = spec;
            mRandom = random;
            Key released = mGate.init(key);
            if (released != null) mDelegate = delegate(released);
            mInitialized = true;
        }

        private Cipher delegate(Key key) throws InvalidKeyException,
                InvalidAlgorithmParameterException {
            try {
                Cipher cipher = other();
                // WRAP and UNWRAP are done here, over encryption and decryption.
                int mode = mOpmode == Cipher.WRAP_MODE ? Cipher.ENCRYPT_MODE
                        : mOpmode == Cipher.UNWRAP_MODE ? Cipher.DECRYPT_MODE : mOpmode;
                if (mSpec != null) {
                    cipher.init(mode, key, mSpec, mRandom != null ? mRandom : new SecureRandom());
                } else {
                    cipher.init(mode, key, mRandom != null ? mRandom : new SecureRandom());
                }
                return cipher;
            } catch (NoSuchAlgorithmException | NoSuchPaddingException e) {
                throw new InvalidKeyException("No provider for " + mTransformation, e);
            }
        }

        /**
         * The transformation from the first other provider that has it, whether it
         * registers the transformation or the algorithm with modes and paddings.
         */
        private Cipher other() throws NoSuchAlgorithmException, NoSuchPaddingException {
            for (Provider provider : java.security.Security.getProviders()) {
                if (provider instanceof AndroidKeyStore) continue;
                try {
                    return Cipher.getInstance(mTransformation, provider);
                } catch (NoSuchAlgorithmException | NoSuchPaddingException next) {
                    // The next provider.
                }
            }
            throw new NoSuchAlgorithmException("Cipher " + mTransformation + " not available");
        }

        /** The working cipher, or null with the reason in mCachedException. */
        private Cipher ready() {
            if (mDelegate != null) return mDelegate;
            if (mCachedException != null) return null;
            if (!mInitialized) throw new IllegalStateException("Not initialized");
            try {
                mDelegate = delegate(mGate.perUse() ? mGate.take() : mGate.release());
            } catch (android.security.KeyStoreException | GeneralSecurityException e) {
                mCachedException = e;
            }
            return mDelegate;
        }

        /** After doFinal the keystore operation is over; the next use is a new one. */
        private void finished() {
            mDelegate = null;
        }

        private IllegalBlockSizeException failed() {
            return (IllegalBlockSizeException)
                    new IllegalBlockSizeException().initCause(mCachedException);
        }

        @Override
        protected byte[] engineUpdate(byte[] input, int offset, int length) {
            Cipher cipher = ready();
            return cipher != null ? cipher.update(input, offset, length) : null;
        }

        @Override
        protected int engineUpdate(byte[] input, int offset, int length, byte[] output,
                int outputOffset) throws ShortBufferException {
            Cipher cipher = ready();
            return cipher != null ? cipher.update(input, offset, length, output, outputOffset) : 0;
        }

        @Override
        protected void engineUpdateAAD(byte[] input, int offset, int length) {
            Cipher cipher = ready();
            if (cipher != null) cipher.updateAAD(input, offset, length);
        }

        @Override
        protected byte[] engineDoFinal(byte[] input, int offset, int length)
                throws IllegalBlockSizeException, BadPaddingException {
            Cipher cipher = ready();
            if (cipher == null) throw failed();
            try {
                return input != null ? cipher.doFinal(input, offset, length) : cipher.doFinal();
            } finally {
                finished();
            }
        }

        @Override
        protected int engineDoFinal(byte[] input, int offset, int length, byte[] output,
                int outputOffset) throws ShortBufferException, IllegalBlockSizeException,
                BadPaddingException {
            byte[] result = engineDoFinal(input, offset, length);
            if (output.length - outputOffset < result.length) {
                throw new ShortBufferException("Output buffer too small");
            }
            System.arraycopy(result, 0, output, outputOffset, result.length);
            return result.length;
        }

        @Override
        protected byte[] engineWrap(Key key) throws IllegalBlockSizeException,
                InvalidKeyException {
            byte[] encoded = key != null ? key.getEncoded() : null;
            if (encoded == null) throw new InvalidKeyException("The key has no encoding");
            try {
                return engineDoFinal(encoded, 0, encoded.length);
            } catch (BadPaddingException e) {
                throw (IllegalBlockSizeException) new IllegalBlockSizeException().initCause(e);
            }
        }

        @Override
        protected Key engineUnwrap(byte[] wrapped, String algorithm, int type)
                throws InvalidKeyException, NoSuchAlgorithmException {
            byte[] encoded;
            try {
                encoded = engineDoFinal(wrapped, 0, wrapped.length);
            } catch (IllegalBlockSizeException | BadPaddingException e) {
                throw new InvalidKeyException("Failed to unwrap key", e);
            }
            try {
                switch (type) {
                    case Cipher.SECRET_KEY:
                        return new SecretKeySpec(encoded, algorithm);
                    case Cipher.PRIVATE_KEY:
                        return KeyFactory.getInstance(algorithm)
                                .generatePrivate(new PKCS8EncodedKeySpec(encoded));
                    case Cipher.PUBLIC_KEY:
                        return KeyFactory.getInstance(algorithm).generatePublic(
                                new java.security.spec.X509EncodedKeySpec(encoded));
                    default:
                        throw new InvalidParameterException("Unsupported wrapped key type: "
                                + type);
                }
            } catch (java.security.spec.InvalidKeySpecException e) {
                throw new InvalidKeyException("Failed to create the unwrapped key", e);
            }
        }

        @Override
        protected int engineGetBlockSize() {
            return aes() ? 16 : 0;
        }

        @Override
        protected int engineGetOutputSize(int inputLength) {
            if (mDelegate != null) return mDelegate.getOutputSize(inputLength);
            return aes() ? inputLength + 32 : Math.max(inputLength, 1024);
        }

        @Override
        protected byte[] engineGetIV() {
            if (mDelegate != null) return mDelegate.getIV();
            if (mSpec instanceof GCMParameterSpec) return ((GCMParameterSpec) mSpec).getIV();
            if (mSpec instanceof IvParameterSpec) return ((IvParameterSpec) mSpec).getIV();
            return null;
        }

        @Override
        protected AlgorithmParameters engineGetParameters() {
            if (mDelegate != null) return mDelegate.getParameters();
            if (mSpec == null || !aes()) return null;
            try {
                AlgorithmParameters params = AlgorithmParameters.getInstance(gcm() ? "GCM" : "AES");
                params.init(mSpec);
                return params;
            } catch (GeneralSecurityException e) {
                throw new ProviderException("Failed to obtain the cipher's parameters", e);
            }
        }

        @Override
        protected int engineGetKeySize(Key key) throws InvalidKeyException {
            if (key instanceof RSAKey) return ((RSAKey) key).getModulus().bitLength();
            throw new UnsupportedOperationException();
        }
    }

    // --- Signature ------------------------------------------------------------------------

    /** A Signature over a sealed private key: AndroidKeyStoreSignatureSpiBase's behavior. */
    public static final class AuthSignature extends SignatureSpi
            implements KeyStoreCryptoOperation {
        private final String mAlgorithm;
        private final Gate mGate = new Gate();
        private Signature mDelegate;
        private AlgorithmParameterSpec mSpec;
        private boolean mInitialized;
        private Exception mCachedException;

        AuthSignature(String algorithm) {
            mAlgorithm = algorithm;
        }

        @Override
        public long getOperationHandle() {
            return mGate.handle();
        }

        @Override
        protected void engineInitVerify(PublicKey publicKey) throws InvalidKeyException {
            throw new InvalidKeyException("Unsupported key: a public key is used with the"
                    + " provider of its own type");
        }

        @Override
        protected void engineInitSign(PrivateKey privateKey) throws InvalidKeyException {
            mInitialized = false;
            mDelegate = null;
            mCachedException = null;
            Key released = mGate.init(privateKey);
            if (released != null) mDelegate = delegate(released);
            mInitialized = true;
        }

        private Signature delegate(Key key) throws InvalidKeyException {
            try {
                Signature signature = Signature.getInstance(mAlgorithm,
                        AndroidKeyStore.other("Signature", mAlgorithm));
                if (mSpec != null) signature.setParameter(mSpec);
                signature.initSign((PrivateKey) key);
                return signature;
            } catch (NoSuchAlgorithmException | InvalidAlgorithmParameterException e) {
                throw new InvalidKeyException("No provider for " + mAlgorithm, e);
            }
        }

        private Signature ready() throws SignatureException {
            if (mDelegate != null) return mDelegate;
            if (mCachedException == null) {
                if (!mInitialized) throw new SignatureException("Not initialized");
                try {
                    mDelegate = delegate(mGate.perUse() ? mGate.take() : mGate.release());
                    return mDelegate;
                } catch (android.security.KeyStoreException | GeneralSecurityException e) {
                    mCachedException = e;
                }
            }
            throw new SignatureException(mCachedException);
        }

        @Override
        protected void engineUpdate(byte b) throws SignatureException {
            ready().update(b);
        }

        @Override
        protected void engineUpdate(byte[] b, int offset, int length) throws SignatureException {
            ready().update(b, offset, length);
        }

        @Override
        protected byte[] engineSign() throws SignatureException {
            Signature signature = ready();
            try {
                return signature.sign();
            } finally {
                mDelegate = null;
            }
        }

        @Override
        protected boolean engineVerify(byte[] signature) throws SignatureException {
            throw new SignatureException("Not initialized for verification");
        }

        @Override
        protected void engineSetParameter(AlgorithmParameterSpec params)
                throws InvalidAlgorithmParameterException {
            mSpec = params;
            if (mDelegate != null) mDelegate.setParameter(params);
        }

        @Override
        @Deprecated
        protected void engineSetParameter(String param, Object value) {
            throw new InvalidParameterException("Unsupported parameter: " + param);
        }

        @Override
        @Deprecated
        protected Object engineGetParameter(String param) {
            throw new InvalidParameterException("Unsupported parameter: " + param);
        }
    }

    // --- Mac ------------------------------------------------------------------------------

    /** A Mac over a sealed HMAC key: AndroidKeyStoreHmacSpi's behavior. */
    public static final class AuthMac extends MacSpi implements KeyStoreCryptoOperation {
        private final String mAlgorithm;
        private final Gate mGate = new Gate();
        private Mac mDelegate;
        private boolean mInitialized;
        private Exception mCachedException;

        AuthMac(String algorithm) {
            mAlgorithm = algorithm;
        }

        @Override
        public long getOperationHandle() {
            return mGate.handle();
        }

        @Override
        protected int engineGetMacLength() {
            switch (mAlgorithm) {
                case "HmacSHA1": return 20;
                case "HmacSHA224": return 28;
                case "HmacSHA256": return 32;
                case "HmacSHA384": return 48;
                default: return 64;
            }
        }

        @Override
        protected void engineInit(Key key, AlgorithmParameterSpec params)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            if (params != null) {
                throw new InvalidAlgorithmParameterException("Unsupported algorithm parameters: "
                        + params);
            }
            mInitialized = false;
            mDelegate = null;
            mCachedException = null;
            Key released = mGate.init(key);
            if (released != null) mDelegate = delegate(released);
            mInitialized = true;
        }

        private Mac delegate(Key key) throws InvalidKeyException {
            try {
                Mac mac = Mac.getInstance(mAlgorithm, AndroidKeyStore.other("Mac", mAlgorithm));
                mac.init(key);
                return mac;
            } catch (NoSuchAlgorithmException e) {
                throw new InvalidKeyException("No provider for " + mAlgorithm, e);
            }
        }

        private Mac ready() {
            if (mDelegate != null) return mDelegate;
            if (mCachedException == null) {
                if (!mInitialized) throw new IllegalStateException("Not initialized");
                try {
                    mDelegate = delegate(mGate.perUse() ? mGate.take() : mGate.release());
                    return mDelegate;
                } catch (android.security.KeyStoreException | GeneralSecurityException e) {
                    mCachedException = e;
                }
            }
            throw new ProviderException("Keystore operation failed", mCachedException);
        }

        @Override
        protected void engineUpdate(byte input) {
            ready().update(input);
        }

        @Override
        protected void engineUpdate(byte[] input, int offset, int length) {
            ready().update(input, offset, length);
        }

        @Override
        protected byte[] engineDoFinal() {
            Mac mac = ready();
            try {
                return mac.doFinal();
            } finally {
                mDelegate = null;
            }
        }

        @Override
        protected void engineReset() {
            if (mDelegate != null) mDelegate.reset();
        }
    }
}
