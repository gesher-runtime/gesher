/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.MessageDigest;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.Arrays;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.function.LongSupplier;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * The user-authentication half of AndroidKeyStore (ADR 0092): what keystore2's
 * enforcement and auth-token table do on a device, over a Windows Hello key.
 *
 * <p>An app's key that requires user authentication is stored by the app's keystore
 * ({@code com.gesher.app.AndroidKeyStore}) only as a blob sealed here: ECIES to the
 * <em>binding</em> public key (ephemeral ECDH P-256, HKDF-SHA256, AES-256-GCM, the key's
 * authorizations as associated data). The binding private key is stored encrypted under
 * a key derived from a Windows Hello signature over the binding's salt
 * ({@link HelloKeyBackend}); every recovery of it is a new signature, which Windows gives
 * only after the user's Windows Hello gesture. The signature must verify as
 * RSASSA-PKCS1-v1_5/SHA-256 under the Hello key's recorded public key, the proof that it
 * is the unique signature of that message; otherwise no binding is made.
 *
 * <p>Android's rules on top of that: a key with a timeout is released while the last
 * confirmation is no older than its timeout; a key without one is released to one
 * operation, after a confirmation made for that operation's handle
 * ({@code BiometricPrompt.CryptoObject}); a binding whose Hello key Windows no longer has
 * (a Windows Hello reset) is gone, and its keys are permanently invalidated. A
 * confirmation counts as the device credential only.
 *
 * <p>Software only: once released, key material is in this process's and the app's
 * memory. See the ADR for the threat model.
 */
public final class KeystoreAuthorization implements KeystoreOperations {
    // Answers to the app's keystore.
    public static final int OK = 0;
    /** Android's KEY_USER_NOT_AUTHENTICATED. */
    public static final int NOT_AUTHENTICATED = 1;
    /** Android's KEY_PERMANENTLY_INVALIDATED: the binding the key was sealed to is gone. */
    public static final int INVALIDATED = 2;
    /** No Windows Hello for this user (or no natives): Android without a secure lock screen. */
    public static final int UNAVAILABLE = 3;
    public static final int CANCELED = 4;
    public static final int NOT_FOREGROUND = 5;
    public static final int FAILED = 6;

    /** KeyProperties.AUTH_DEVICE_CREDENTIAL: what a Windows Hello confirmation counts as. */
    public static final int AUTH_DEVICE_CREDENTIAL = 1;

    /**
     * {@link #authorize}'s result when there is nothing to sign with, known before any
     * prompt was shown. The other results are {@link WindowsHelloBackend}'s.
     */
    public static final int NO_BINDING = -2;

    /** Operations waiting for authorization, per uid (keystore2 prunes the oldest too). */
    static final int MAX_OPERATIONS_PER_UID = 16;
    private static final long LIVENESS_CACHE_MS = 5000;

    private static final int BINDING_MAGIC = 0x474b4231;  // "GKB1"
    private static final int BLOB_MAGIC = 0x474b5731;     // "GKW1"
    private static final byte[] SIGN_CONTEXT =
            "Gesher AndroidKeyStore binding v1\0".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] KEK_INFO =
            "Gesher AndroidKeyStore binding KEK v1".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] WRAP_INFO =
            "Gesher AndroidKeyStore key wrap v1".getBytes(StandardCharsets.US_ASCII);
    private static final String NAME_PREFIX = "Gesher AndroidKeyStore ";

    /** An answer: a status and, with OK, the blob, the key material or the handle. */
    public static final class Reply {
        public final int status;
        public final byte[] data;
        public final long handle;

        Reply(int status, byte[] data, long handle) {
            this.status = status;
            this.data = data;
            this.handle = handle;
        }

        static Reply of(int status) {
            return new Reply(status, null, 0);
        }
    }

    /** The binding record, as stored. */
    private static final class Binding {
        byte[] id;            // 16 random bytes; also in every blob sealed to it
        String name;          // the Hello key's name in Windows
        byte[] salt;          // 32 random bytes, signed
        byte[] helloPublic;   // the Hello key's SubjectPublicKeyInfo at enrollment
        byte[] bindingPublic; // EC P-256 SubjectPublicKeyInfo
        byte[] iv;
        byte[] sealedPrivate; // the binding private key (PKCS#8), AES-256-GCM
        int maxTimeout;       // the longest timeout of a key sealed to it, seconds
    }

    /** One begun operation of a key without a timeout. */
    private static final class Op {
        final int uid;
        final Object owner;
        final byte[] blob;
        byte[] material;  // set once authorized

        Op(int uid, Object owner, byte[] blob) {
            this.uid = uid;
            this.owner = owner;
            this.blob = blob;
        }
    }

    private final HelloKeyBackend mBackend;  // null: no Windows Hello natives
    private final File mFile;
    private final LongSupplier mClock;       // elapsed milliseconds, monotonic
    private final SecureRandom mRandom = new SecureRandom();
    private final Object mLock = new Object();
    private final Object mEnrollLock = new Object();

    // Guarded by mLock.
    private boolean mLoaded;
    private Binding mBinding;
    private PrivateKey mUnlocked;   // the binding private key, while a timeout may need it
    private long mUnlockedAt;
    private long mLiveAt = Long.MIN_VALUE;
    private final LinkedHashMap<Long, Op> mOps = new LinkedHashMap<>();

    public KeystoreAuthorization(HelloKeyBackend backend, File file, LongSupplier elapsedMillis) {
        mBackend = backend;
        mFile = file;
        mClock = elapsedMillis;
    }

    // --- the app's keystore ---------------------------------------------------------------

    /** OK when keys that require user authentication can be made, else UNAVAILABLE. */
    public int availability() {
        if (mBackend == null) return UNAVAILABLE;
        synchronized (mLock) {
            if (load() != null) return OK;
        }
        return mBackend.isSupported() ? OK : UNAVAILABLE;
    }

    /**
     * Seals {@code material} with its authorizations. The first key of the device enrolls
     * the binding, which asks the user for Windows Hello over {@code pid}'s window.
     */
    public Reply seal(int pid, byte[] material, int timeoutSeconds, int authType) {
        if (mBackend == null) return Reply.of(UNAVAILABLE);
        if (material == null || timeoutSeconds < 0 || (authType & AUTH_DEVICE_CREDENTIAL) == 0) {
            return Reply.of(FAILED);  // a biometric-only key cannot be satisfied here
        }
        synchronized (mEnrollLock) {
            Binding binding = live();
            if (binding == null) {
                int status = enroll(pid);
                if (status != OK) return Reply.of(status);
            }
            synchronized (mLock) {
                binding = mBinding;
                if (binding == null) return Reply.of(FAILED);
                try {
                    if (timeoutSeconds > binding.maxTimeout) {
                        binding.maxTimeout = timeoutSeconds;
                        store(binding);
                    }
                    unlocked();  // not kept after enrolling unless a timeout needs it
                    return new Reply(OK, sealBlob(binding, material, timeoutSeconds, authType), 0);
                } catch (GeneralSecurityException | IOException e) {
                    log("sealing a key failed: " + e);
                    return Reply.of(FAILED);
                }
            }
        }
    }

    /** The material of a key with a timeout, while the last confirmation is recent enough. */
    public Reply release(byte[] blob) {
        Header header = Header.parse(blob);
        if (header == null || header.timeout <= 0) return Reply.of(FAILED);
        Binding binding = live();
        synchronized (mLock) {
            if (binding == null || binding != mBinding || !Arrays.equals(binding.id, header.id)) {
                return Reply.of(INVALIDATED);
            }
            PrivateKey key = unlocked();
            // keystore2's authorize_create: token_age.seconds() > key_time_out.
            if (key == null || (mClock.getAsLong() - mUnlockedAt) / 1000 > header.timeout) {
                return Reply.of(NOT_AUTHENTICATED);
            }
            byte[] material = openBlob(key, header, blob);
            return material != null ? new Reply(OK, material, 0) : Reply.of(FAILED);
        }
    }

    /**
     * Begins an operation of a key without a timeout: its handle, what
     * {@code CryptoObject.getOpId()} gives BiometricPrompt. {@code owner} identifies the
     * client for {@link #clientDied}.
     */
    public Reply begin(int uid, Object owner, byte[] blob) {
        Header header = Header.parse(blob);
        if (header == null || header.timeout != 0) return Reply.of(FAILED);
        Binding binding = live();
        synchronized (mLock) {
            if (binding == null || binding != mBinding || !Arrays.equals(binding.id, header.id)) {
                return Reply.of(INVALIDATED);
            }
            long handle;
            do {
                handle = mRandom.nextLong();
            } while (handle == 0 || mOps.containsKey(handle));
            int count = 0;
            for (Op op : mOps.values()) {
                if (op.uid == uid) count++;
            }
            for (Iterator<Op> i = mOps.values().iterator();
                    count >= MAX_OPERATIONS_PER_UID && i.hasNext(); ) {
                Op old = i.next();
                if (old.uid != uid) continue;
                wipe(old.material);
                i.remove();
                count--;
            }
            mOps.put(handle, new Op(uid, owner, blob.clone()));
            return new Reply(OK, null, handle);
        }
    }

    /**
     * The material for a begun operation, once: OK after its authorization,
     * NOT_AUTHENTICATED before. Either way the operation is over, as keystore2 ends an
     * operation on its first error.
     */
    public Reply finish(int uid, long handle) {
        synchronized (mLock) {
            Op op = mOps.get(handle);
            if (op == null || op.uid != uid) return Reply.of(FAILED);
            mOps.remove(handle);
            if (op.material == null) return Reply.of(NOT_AUTHENTICATED);
            return new Reply(OK, op.material, 0);
        }
    }

    public void abort(int uid, long handle) {
        synchronized (mLock) {
            Op op = mOps.get(handle);
            if (op == null || op.uid != uid) return;
            wipe(op.material);
            mOps.remove(handle);
        }
    }

    /** The client that began operations as {@code owner} is gone. */
    public void clientDied(Object owner) {
        synchronized (mLock) {
            for (Iterator<Op> i = mOps.values().iterator(); i.hasNext(); ) {
                Op op = i.next();
                if (op.owner != owner) continue;
                wipe(op.material);
                i.remove();
            }
        }
    }

    // --- the auth service -----------------------------------------------------------------

    @Override
    public boolean isPending(int uid, long operationId) {
        synchronized (mLock) {
            Op op = mOps.get(operationId);
            return op != null && op.uid == uid && op.material == null;
        }
    }

    /**
     * Whether a credential confirmation should be this binding's signature, so that keys
     * with a timeout are released by it: there is a binding and it has such keys.
     */
    public boolean confirmsWithKey() {
        if (mBackend == null) return false;
        synchronized (mLock) {
            Binding binding = load();
            return binding != null && binding.maxTimeout > 0;
        }
    }

    /**
     * Asks the user for Windows Hello through the binding's key. VERIFIED only when Windows
     * signed, the signature recovered the binding key and, for {@code operationId != 0},
     * that operation of {@code request.uid} got its key.
     */
    @Override
    public WindowsHelloBackend.Verification authorize(WindowsHelloBackend.Request request,
            long operationId, WindowsHelloBackend.Completion completion) {
        Binding binding = null;
        if (mBackend != null) {
            synchronized (mLock) {
                binding = load();
            }
        }
        if (binding == null) {
            // The interface forbids completing before returning.
            Thread t = new Thread(() -> completion.onResult(NO_BINDING), "keystore-auth");
            t.setDaemon(true);
            t.start();
            return () -> { };
        }
        final Binding signed = binding;
        HelloKeyBackend.Operation operation = mBackend.sign(binding.name, request.pid,
                message(binding), result -> completion.onResult(
                        signed(signed, request.uid, operationId, result)));
        return operation::cancel;
    }

    private int signed(Binding binding, int uid, long operationId, HelloKeyBackend.Result result) {
        switch (result.status) {
            case HelloKeyBackend.SUCCESS:
                break;
            case HelloKeyBackend.USER_CANCELED:
            case HelloKeyBackend.NOT_FOREGROUND:
                return WindowsHelloBackend.CANCELED;
            case HelloKeyBackend.NOT_FOUND:
                invalidate(binding, "Windows no longer has its Windows Hello key");
                return NO_BINDING;
            default:
                return WindowsHelloBackend.FAILED;
        }
        PrivateKey key = unlock(binding, result.data);
        if (key == null) {
            invalidate(binding, "the Windows Hello key no longer gives the binding's signature");
            return WindowsHelloBackend.FAILED;
        }
        synchronized (mLock) {
            if (binding != mBinding) return WindowsHelloBackend.FAILED;
            // keystore2 finds a time-bound key's token by time, whatever its challenge.
            if (binding.maxTimeout > 0) {
                mUnlocked = key;
                mUnlockedAt = mClock.getAsLong();
            }
            if (operationId == 0) return WindowsHelloBackend.VERIFIED;
            Op op = mOps.get(operationId);
            if (op == null || op.uid != uid || op.material != null) {
                return WindowsHelloBackend.FAILED;
            }
            Header header = Header.parse(op.blob);
            byte[] material = header != null ? openBlob(key, header, op.blob) : null;
            if (material == null) return WindowsHelloBackend.FAILED;
            op.material = material;
            return WindowsHelloBackend.VERIFIED;
        }
    }

    // --- the binding ----------------------------------------------------------------------

    /** The stored binding. Call with mLock held. */
    private Binding load() {
        if (!mLoaded) {
            mLoaded = true;
            mBinding = read();
        }
        return mBinding;
    }

    /** The binding private key if a timeout may still need it. Call with mLock held. */
    private PrivateKey unlocked() {
        if (mUnlocked != null && (mBinding == null || mBinding.maxTimeout == 0
                || (mClock.getAsLong() - mUnlockedAt) / 1000 > mBinding.maxTimeout)) {
            mUnlocked = null;
        }
        return mUnlocked;
    }

    /**
     * The binding, if Windows still has its Hello key with the recorded public key (asked
     * at most every {@value #LIVENESS_CACHE_MS} ms, without UI). A binding whose key is
     * gone is invalidated: null.
     */
    private Binding live() {
        if (mBackend == null) return null;
        Binding binding;
        synchronized (mLock) {
            binding = load();
            if (binding == null) return null;
            if (mLiveAt != Long.MIN_VALUE && mClock.getAsLong() - mLiveAt < LIVENESS_CACHE_MS) {
                return binding;
            }
        }
        HelloKeyBackend.Result opened = mBackend.open(binding.name);
        if (opened.status == HelloKeyBackend.NOT_FOUND) {
            invalidate(binding, "Windows no longer has its Windows Hello key");
            return null;
        }
        if (opened.status == HelloKeyBackend.SUCCESS
                && !Arrays.equals(opened.data, binding.helloPublic)) {
            invalidate(binding, "its Windows Hello key was replaced");
            return null;
        }
        synchronized (mLock) {
            // Another failure (Windows busy, an HRESULT) says nothing about the key.
            if (opened.status == HelloKeyBackend.SUCCESS) mLiveAt = mClock.getAsLong();
            return binding == mBinding ? binding : null;
        }
    }

    private void invalidate(Binding binding, String why) {
        synchronized (mLock) {
            if (binding != mBinding) return;
            log("the keystore binding is invalidated (" + why
                    + "); keys that require user authentication are permanently invalidated");
            mBinding = null;
            mUnlocked = null;
            mLiveAt = Long.MIN_VALUE;
            for (Op op : mOps.values()) wipe(op.material);
            mOps.clear();
            if (!mFile.delete() && mFile.exists()) log("cannot delete " + mFile);
        }
    }

    /**
     * Creates the Windows Hello key and the binding: two Windows Hello prompts (creating
     * the key, then its first signature). Call with mEnrollLock held.
     */
    private int enroll(int pid) {
        if (!mBackend.isSupported()) return UNAVAILABLE;
        Binding binding = new Binding();
        binding.id = random(16);
        binding.salt = random(32);
        binding.name = NAME_PREFIX + hex(binding.id);
        HelloKeyBackend.Result created = mBackend.create(binding.name, pid);
        if (created.status != HelloKeyBackend.SUCCESS) return enrollFailure(created.status);
        binding.helloPublic = created.data;
        boolean kept = false;
        try {
            CountDownLatch done = new CountDownLatch(1);
            HelloKeyBackend.Result[] signed = new HelloKeyBackend.Result[1];
            mBackend.sign(binding.name, pid, message(binding), result -> {
                signed[0] = result;
                done.countDown();
            });
            try {
                done.await();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return FAILED;
            }
            if (signed[0].status != HelloKeyBackend.SUCCESS) return enrollFailure(signed[0].status);
            byte[] kek = kek(binding, signed[0].data);
            if (kek == null) {
                log("Windows Hello's signature is not RSASSA-PKCS1-v1_5/SHA-256 under its key:"
                        + " no binding is made");
                return FAILED;
            }
            KeyPairGenerator generator = KeyPairGenerator.getInstance("EC");
            generator.initialize(new ECGenParameterSpec("secp256r1"), mRandom);
            KeyPair pair = generator.generateKeyPair();
            binding.bindingPublic = pair.getPublic().getEncoded();
            binding.iv = random(12);
            Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
            gcm.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(kek, "AES"),
                    new GCMParameterSpec(128, binding.iv));
            gcm.updateAAD(binding.id);
            binding.sealedPrivate = gcm.doFinal(pair.getPrivate().getEncoded());
            synchronized (mLock) {
                store(binding);
                mLoaded = true;
                mBinding = binding;
                // The user has just confirmed with Windows Hello: a real confirmation.
                mUnlocked = pair.getPrivate();
                mUnlockedAt = mClock.getAsLong();
                mLiveAt = mUnlockedAt;
            }
            kept = true;
            return OK;
        } catch (GeneralSecurityException | IOException e) {
            log("enrolling the keystore binding failed: " + e);
            return FAILED;
        } finally {
            if (!kept) mBackend.delete(binding.name);
        }
    }

    private static int enrollFailure(int status) {
        switch (status) {
            case HelloKeyBackend.USER_CANCELED:
                return CANCELED;
            case HelloKeyBackend.NOT_FOREGROUND:
                return NOT_FOREGROUND;
            default:
                return FAILED;
        }
    }

    private static byte[] message(Binding binding) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(SIGN_CONTEXT, 0, SIGN_CONTEXT.length);
        out.write(binding.id, 0, binding.id.length);
        out.write(binding.salt, 0, binding.salt.length);
        return out.toByteArray();
    }

    /** DER of DigestInfo for SHA-256 up to the digest (RFC 8017 section 9.2, note 1). */
    private static final byte[] SHA256_DIGEST_INFO = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
            (byte) 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

    /**
     * The key-encryption key from Windows Hello's signature, or null unless the signature
     * is the RSASSA-PKCS1-v1_5/SHA-256 signature of the binding's message under the
     * recorded RSA public key of at least 2048 bits. The check is exact: signature^e mod n
     * must equal the one encoded message 00 01 FF..FF 00 DigestInfo(SHA-256(message)). RSA
     * is a permutation, so exactly one value passes: the key gives this signature for this
     * message every time, which is what lets it stand in for a stored secret. A randomized
     * signature (PSS) never passes, and then no binding is made.
     */
    private static byte[] kek(Binding binding, byte[] signature) {
        if (signature == null) return null;
        try {
            PublicKey hello = KeyFactory.getInstance("RSA")
                    .generatePublic(new X509EncodedKeySpec(binding.helloPublic));
            if (!(hello instanceof RSAPublicKey)) return null;
            BigInteger n = ((RSAPublicKey) hello).getModulus();
            int k = (n.bitLength() + 7) / 8;
            BigInteger s = new BigInteger(1, signature);
            if (n.bitLength() < 2048 || signature.length != k || s.compareTo(n) >= 0) return null;
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(message(binding));
            byte[] expected = new byte[k];
            int t = SHA256_DIGEST_INFO.length + digest.length;
            expected[1] = 1;
            Arrays.fill(expected, 2, k - t - 1, (byte) 0xff);
            System.arraycopy(SHA256_DIGEST_INFO, 0, expected, k - t, SHA256_DIGEST_INFO.length);
            System.arraycopy(digest, 0, expected, k - digest.length, digest.length);
            byte[] actual = s.modPow(((RSAPublicKey) hello).getPublicExponent(), n).toByteArray();
            if (!new BigInteger(1, expected).equals(new BigInteger(1, actual))) return null;
            return hkdf(signature, binding.salt, KEK_INFO);
        } catch (GeneralSecurityException | RuntimeException e) {
            return null;
        }
    }

    /** The binding private key from a new signature, or null. */
    private static PrivateKey unlock(Binding binding, byte[] signature) {
        byte[] kek = kek(binding, signature);
        if (kek == null) return null;
        try {
            Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
            gcm.init(Cipher.DECRYPT_MODE, new SecretKeySpec(kek, "AES"),
                    new GCMParameterSpec(128, binding.iv));
            gcm.updateAAD(binding.id);
            byte[] pkcs8 = gcm.doFinal(binding.sealedPrivate);
            try {
                return KeyFactory.getInstance("EC").generatePrivate(new PKCS8EncodedKeySpec(pkcs8));
            } finally {
                wipe(pkcs8);
            }
        } catch (GeneralSecurityException | RuntimeException e) {
            return null;
        } finally {
            wipe(kek);
        }
    }

    // --- blobs ----------------------------------------------------------------------------

    /** A blob's authenticated header: binding, authorizations, ephemeral key, IV. */
    private static final class Header {
        byte[] id;
        int timeout;
        int authType;
        byte[] ephemeral;
        byte[] iv;
        int length;  // of the header in the blob: the associated data

        static Header parse(byte[] blob) {
            if (blob == null) return null;
            try (DataInputStream in = new DataInputStream(new ByteArrayInputStream(blob))) {
                Header h = new Header();
                if (in.readInt() != BLOB_MAGIC) return null;
                h.id = new byte[16];
                in.readFully(h.id);
                h.timeout = in.readInt();
                h.authType = in.readInt();
                h.ephemeral = new byte[in.readUnsignedShort()];
                in.readFully(h.ephemeral);
                h.iv = new byte[12];
                in.readFully(h.iv);
                h.length = blob.length - in.available();
                if (h.timeout < 0 || (h.authType & AUTH_DEVICE_CREDENTIAL) == 0) return null;
                return h;
            } catch (IOException e) {
                return null;
            }
        }
    }

    private byte[] sealBlob(Binding binding, byte[] material, int timeout, int authType)
            throws GeneralSecurityException, IOException {
        KeyFactory ec = KeyFactory.getInstance("EC");
        PublicKey recipient = ec.generatePublic(new X509EncodedKeySpec(binding.bindingPublic));
        KeyPairGenerator generator = KeyPairGenerator.getInstance("EC");
        generator.initialize(new ECGenParameterSpec("secp256r1"), mRandom);
        KeyPair ephemeral = generator.generateKeyPair();
        byte[] ephemeralPublic = ephemeral.getPublic().getEncoded();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeInt(BLOB_MAGIC);
        out.write(binding.id);
        out.writeInt(timeout);
        out.writeInt(authType);
        out.writeShort(ephemeralPublic.length);
        out.write(ephemeralPublic);
        byte[] iv = random(12);
        out.write(iv);
        out.flush();
        byte[] header = bytes.toByteArray();
        byte[] key = wrapKey(ephemeral.getPrivate(), recipient, binding.id, ephemeralPublic);
        try {
            Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
            gcm.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"),
                    new GCMParameterSpec(128, iv));
            gcm.updateAAD(header);
            bytes.write(gcm.doFinal(material));
            return bytes.toByteArray();
        } finally {
            wipe(key);
        }
    }

    /** The material in {@code blob}, or null when it does not open (changed, or not ours). */
    private static byte[] openBlob(PrivateKey bindingKey, Header header, byte[] blob) {
        byte[] key = null;
        try {
            PublicKey ephemeral = KeyFactory.getInstance("EC")
                    .generatePublic(new X509EncodedKeySpec(header.ephemeral));
            key = wrapKey(bindingKey, ephemeral, header.id, header.ephemeral);
            Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
            gcm.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"),
                    new GCMParameterSpec(128, header.iv));
            gcm.updateAAD(blob, 0, header.length);
            return gcm.doFinal(blob, header.length, blob.length - header.length);
        } catch (GeneralSecurityException | RuntimeException e) {
            return null;
        } finally {
            wipe(key);
        }
    }

    private static byte[] wrapKey(PrivateKey mine, PublicKey theirs, byte[] bindingId,
            byte[] ephemeralPublic) throws GeneralSecurityException {
        KeyAgreement ecdh = KeyAgreement.getInstance("ECDH");
        ecdh.init(mine);
        ecdh.doPhase(theirs, true);
        byte[] shared = ecdh.generateSecret();
        try {
            byte[] info = Arrays.copyOf(WRAP_INFO, WRAP_INFO.length + ephemeralPublic.length);
            System.arraycopy(ephemeralPublic, 0, info, WRAP_INFO.length, ephemeralPublic.length);
            return hkdf(shared, bindingId, info);
        } finally {
            wipe(shared);
        }
    }

    /** HKDF-SHA256 (RFC 5869), one block: 32 bytes. */
    static byte[] hkdf(byte[] ikm, byte[] salt, byte[] info) throws GeneralSecurityException {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(salt, "HmacSHA256"));
        byte[] prk = mac.doFinal(ikm);
        try {
            mac.init(new SecretKeySpec(prk, "HmacSHA256"));
            mac.update(info);
            mac.update((byte) 1);
            return mac.doFinal();
        } finally {
            wipe(prk);
        }
    }

    // --- the record -----------------------------------------------------------------------

    private Binding read() {
        if (!mFile.exists()) return null;
        try (DataInputStream in = new DataInputStream(new java.io.BufferedInputStream(
                new FileInputStream(mFile)))) {
            if (in.readInt() != BINDING_MAGIC) throw new IOException("not a keystore binding");
            Binding b = new Binding();
            b.id = new byte[16];
            in.readFully(b.id);
            b.name = in.readUTF();
            b.salt = bytes(in);
            b.helloPublic = bytes(in);
            b.bindingPublic = bytes(in);
            b.iv = bytes(in);
            b.sealedPrivate = bytes(in);
            b.maxTimeout = in.readInt();
            if (b.salt.length != 32 || b.iv.length != 12 || b.maxTimeout < 0) {
                throw new IOException("malformed keystore binding");
            }
            return b;
        } catch (IOException e) {
            // Unreadable is not "gone": the file is left for a person to look at, and keys
            // sealed to it stay unusable (permanently invalidated to the app).
            log("cannot read " + mFile + ": " + e);
            return null;
        }
    }

    private void store(Binding b) throws IOException {
        File parent = mFile.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        File tmp = new File(mFile.getPath() + ".tmp");
        try (DataOutputStream out = new DataOutputStream(new java.io.BufferedOutputStream(
                new FileOutputStream(tmp)))) {
            out.writeInt(BINDING_MAGIC);
            out.write(b.id);
            out.writeUTF(b.name);
            for (byte[] part : new byte[][] {b.salt, b.helloPublic, b.bindingPublic, b.iv,
                    b.sealedPrivate}) {
                out.writeInt(part.length);
                out.write(part);
            }
            out.writeInt(b.maxTimeout);
        }
        java.nio.file.Files.move(tmp.toPath(), mFile.toPath(),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    private static byte[] bytes(DataInputStream in) throws IOException {
        int n = in.readInt();
        if (n < 0 || n > 1 << 16) throw new IOException("malformed keystore binding");
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }

    private byte[] random(int n) {
        byte[] b = new byte[n];
        mRandom.nextBytes(b);
        return b;
    }

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder(b.length * 2);
        for (byte x : b) s.append(Character.forDigit((x >> 4) & 15, 16)).append(
                Character.forDigit(x & 15, 16));
        return s.toString();
    }

    private static void wipe(byte[] b) {
        if (b != null) Arrays.fill(b, (byte) 0);
    }

    /** How many operations wait or hold a key (diagnostics and tests). */
    int operations() {
        synchronized (mLock) {
            return mOps.size();
        }
    }

    /** Whether the binding private key is in memory now (diagnostics and tests). */
    boolean holdsBindingKey() {
        synchronized (mLock) {
            return unlocked() != null;
        }
    }

    static void log(String message) {
        System.err.println("gesher-keystore: " + message);
    }
}
