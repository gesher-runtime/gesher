/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.hellokeys;

import android.app.Activity;
import android.app.KeyguardManager;
import android.content.Intent;
import android.hardware.biometrics.BiometricManager.Authenticators;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.security.keystore.UserNotAuthenticatedException;
import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * AndroidKeyStore keys that require the device credential, used the way apps use them,
 * printing every answer.
 *
 * <p>{@code $GESHER_HELLOKEYS_MODE}:
 * <ul>
 * <li>{@code answers} (CI): on a device that is not secure, asks for both keys (each must
 *     be refused as Android refuses it without a secure lock screen) and checks that a key
 *     without the requirement still works. On a secure device it makes no key that needs
 *     authentication (the first one asks for Windows Hello) and shows no prompt.
 * <li>unset, or {@code prompt} (by hand, Windows Hello set up): the whole sequence below;
 *     the person at the PC confirms each Windows Hello prompt.
 * <li>{@code persist}: on a fresh test profile, creates two separate protected fixture
 *     keys, requires a real CryptoObject confirmation, and stores a random challenge
 *     encrypted by each key. Refuses to replace an existing fixture or key.
 * <li>{@code reopen}: after shutting down the complete device, reopens those same keys
 *     and ciphertexts without generating or deleting keys. Requires refusal before new
 *     authentication and a fresh CryptoObject confirmation before both decryptions.
 * </ul>
 *
 * <p>The sequence: a per-operation key ({@code setUserAuthenticationParameters(0,
 * AUTH_DEVICE_CREDENTIAL)}) and a time-bound key (30 s) are made; the time-bound key is
 * refused before any confirmation ({@code UserNotAuthenticatedException} from
 * {@code init}); the per-operation key's cipher initializes but cannot be used
 * ({@code IllegalBlockSizeException} from {@code doFinal}, as keystore2's
 * KEY_USER_NOT_AUTHENTICATED); {@code BiometricPrompt} with {@code DEVICE_CREDENTIAL} and
 * a {@code CryptoObject} confirms that cipher, which then encrypts and decrypts; after
 * the timeout the time-bound key is refused again, and after the confirm-credential
 * intent's {@code RESULT_OK} it is used.
 */
public class MainActivity extends Activity {
    private static final String TAG = "hellokeys";
    private static final String PER_OPERATION = "hellokeys-per-operation";
    private static final String TIME_BOUND = "hellokeys-time-bound";
    private static final String PLAIN = "hellokeys-plain";
    private static final int TIMEOUT_SECONDS = 30;
    private static final int CONFIRM_REQUEST = 11;
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final byte[] PLAINTEXT = "Gesher authenticated key".getBytes();
    private static final String PERSIST_PER_OPERATION = "hellokeys-persist-per-operation";
    private static final String PERSIST_TIME_BOUND = "hellokeys-persist-time-bound";
    private static final String PERSIST_FILE = "hellokeys-persist-v1.bin";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mMode;
    private KeyStore mKeyStore;

    private static void line(String message) {
        System.out.println("hellokeys: " + message);
        System.out.flush();
        Log.i(TAG, message);
    }

    private static String describe(Throwable t) {
        StringBuilder text = new StringBuilder(t.getClass().getSimpleName());
        Throwable cause = t.getCause();
        if (cause != null) {
            text.append(", cause ").append(cause.getClass().getName());
            if (cause.getMessage() != null) text.append(": ").append(cause.getMessage());
        }
        return text.toString();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) return;
        String mode = System.getenv("GESHER_HELLOKEYS_MODE");
        mMode = mode == null || mode.isEmpty() ? "prompt" : mode;
        line("mode: " + mMode);
        KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        boolean secure = keyguard.isDeviceSecure();
        line("keyguard: isDeviceSecure " + secure);
        if ("persist".equals(mMode) || "reopen".equals(mMode)) {
            // The first native enrollment also needs the owning activity's window.
            mHandler.post(() -> persistence(secure));
            return;
        }
        try {
            mKeyStore = KeyStore.getInstance("AndroidKeyStore");
            mKeyStore.load(null);
            for (String alias : new String[] {PER_OPERATION, TIME_BOUND, PLAIN}) {
                if (mKeyStore.containsAlias(alias)) mKeyStore.deleteEntry(alias);
            }
        } catch (Exception e) {
            line("keystore: unavailable: " + e);
            done();
            return;
        }
        plain();
        if (secure && "answers".equals(mMode)) {
            line("keys: not generated (the device is secure and nobody is here to answer)");
            done();
            return;
        }
        SecretKey perOperation = generate("per-operation", PER_OPERATION, 0);
        SecretKey timeBound = generate("time-bound", TIME_BOUND, TIMEOUT_SECONDS);
        if (perOperation == null || timeBound == null) {
            done();
            return;
        }
        useTimeBound("before any confirmation", timeBound);
        Cipher unauthenticated = init("per-operation", perOperation);
        if (unauthenticated != null) {
            try {
                unauthenticated.doFinal(PLAINTEXT);
                line("per-operation: doFinal without authentication succeeded");
            } catch (Exception e) {
                line("per-operation: doFinal without authentication " + describe(e));
            }
        }
        Cipher cipher = init("per-operation (for the prompt)", perOperation);
        if (cipher == null) {
            done();
            return;
        }
        // Posted: the prompt needs this activity's window.
        mHandler.post(() -> prompt(perOperation, timeBound, cipher));
    }

    private boolean mPersistenceNewAliases;
    private boolean mPersistenceFinished;

    /** Positive restart evidence uses separate aliases; ordinary prompt/answers never touch them. */
    private void persistence(boolean secure) {
        if (!secure) {
            persistenceFailed("a real enrolled device credential is required");
            return;
        }
        boolean reopen = "reopen".equals(mMode);
        try {
            mKeyStore = KeyStore.getInstance("AndroidKeyStore");
            mKeyStore.load(null);
            File file = new File(getFilesDir(), PERSIST_FILE);
            final SecretKey perOperation;
            final SecretKey timeBound;
            final PersistedState previous;
            final byte[] challenge;
            if (reopen) {
                // Read and validate before any prompt. Never silently create missing state.
                previous = PersistedState.read(file);
                perOperation = existingSecret(PERSIST_PER_OPERATION);
                timeBound = existingSecret(PERSIST_TIME_BOUND);
                challenge = previous.challenge;
                line("persistence: reopening existing keys, challenge " + challengeId(challenge));
            } else {
                if (file.exists() || mKeyStore.containsAlias(PERSIST_PER_OPERATION)
                        || mKeyStore.containsAlias(PERSIST_TIME_BOUND)) {
                    throw new IOException("fixture already exists; use reopen or a fresh profile");
                }
                previous = null;
                challenge = new byte[32];
                new SecureRandom().nextBytes(challenge);
                mPersistenceNewAliases = true;
                perOperation = generate("persist per-operation", PERSIST_PER_OPERATION, 0);
                timeBound = generate("persist time-bound", PERSIST_TIME_BOUND, TIMEOUT_SECONDS);
                if (perOperation == null || timeBound == null) {
                    throw new IOException("both protected fixture keys must be generated");
                }
            }

            // A previous system server's cached authorization must not survive the restart.
            Cipher timed = Cipher.getInstance(TRANSFORMATION);
            try {
                if (reopen) {
                    timed.init(Cipher.DECRYPT_MODE, timeBound,
                            new GCMParameterSpec(128, previous.timeIv));
                } else {
                    timed.init(Cipher.ENCRYPT_MODE, timeBound);
                }
                throw new IOException("time-bound key authorized before a new confirmation");
            } catch (UserNotAuthenticatedException expected) {
                line("persistence: time-bound refused before new confirmation");
            }
            Cipher refused = persistenceCipher(perOperation, previous);
            try {
                refused.doFinal(reopen ? previous.perSealed : challenge);
                throw new IOException("per-operation key used without a new CryptoObject prompt");
            } catch (IllegalBlockSizeException expected) {
                Throwable cause = expected.getCause();
                if (!(cause instanceof android.security.KeyStoreException)
                        || ((android.security.KeyStoreException) cause).getErrorCode() != -26) {
                    throw new IOException("per-operation failed for a reason other than "
                            + "KEY_USER_NOT_AUTHENTICATED", expected);
                }
                line("persistence: per-operation refused before new confirmation: "
                        + describe(expected));
            }

            // The failed operation above is discarded; authenticate a fresh operation.
            Cipher requested = persistenceCipher(perOperation, previous);
            new BiometricPrompt.Builder(this)
                    .setTitle("Gesher persisted-key test")
                    .setDescription(reopen ? "Confirm to reopen the same protected keys"
                            : "Confirm to save protected keys for a restart test")
                    .setAllowedAuthenticators(Authenticators.DEVICE_CREDENTIAL)
                    .build()
                    .authenticate(new BiometricPrompt.CryptoObject(requested),
                            new CancellationSignal(), getMainExecutor(),
                            new BiometricPrompt.AuthenticationCallback() {
                                @Override
                                public void onAuthenticationError(int code, CharSequence text) {
                                    persistenceFailed("credential prompt error " + code);
                                }

                                @Override
                                public void onAuthenticationSucceeded(
                                        BiometricPrompt.AuthenticationResult result) {
                                    if (mPersistenceFinished) return;
                                    if (result.getAuthenticationType() != 1
                                            || result.getCryptoObject() == null
                                            || result.getCryptoObject().getCipher() != requested) {
                                        persistenceFailed("unexpected authentication result");
                                        return;
                                    }
                                    persistenceAuthorized(file, previous, challenge, requested,
                                            timeBound);
                                }

                                @Override
                                public void onAuthenticationFailed() {
                                    line("persistence: credential attempt failed; still waiting");
                                }
                            });
            line("persistence: fresh CryptoObject prompt started");
        } catch (Exception e) {
            persistenceFailed(describe(e) + ": " + e.getMessage());
        }
    }

    private SecretKey existingSecret(String alias) throws Exception {
        if (!mKeyStore.containsAlias(alias)) throw new IOException("missing fixture alias " + alias);
        java.security.Key key = mKeyStore.getKey(alias, null);
        if (!(key instanceof SecretKey)) throw new IOException("fixture alias is not a secret key");
        return (SecretKey) key;
    }

    private static Cipher persistenceCipher(SecretKey key, PersistedState previous) throws Exception {
        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        if (previous == null) {
            cipher.init(Cipher.ENCRYPT_MODE, key);
        } else {
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, previous.perIv));
        }
        return cipher;
    }

    private void persistenceAuthorized(File file, PersistedState previous, byte[] challenge,
            Cipher perOperation, SecretKey timeBound) {
        try {
            Cipher timed = Cipher.getInstance(TRANSFORMATION);
            if (previous == null) {
                byte[] perSealed = perOperation.doFinal(challenge);
                timed.init(Cipher.ENCRYPT_MODE, timeBound);
                byte[] timeSealed = timed.doFinal(challenge);
                PersistedState saved = new PersistedState(challenge, perOperation.getIV(),
                        perSealed, timed.getIV(), timeSealed);
                saved.writeNew(file);
                if (!saved.sameAs(PersistedState.read(file))) {
                    throw new IOException("persisted fixture readback differs");
                }
                mPersistenceNewAliases = false;
                line("persistence: stored both ciphertexts, challenge " + challengeId(challenge));
            } else {
                if (!Arrays.equals(challenge, perOperation.doFinal(previous.perSealed))) {
                    throw new IOException("per-operation persisted challenge differs");
                }
                timed.init(Cipher.DECRYPT_MODE, timeBound,
                        new GCMParameterSpec(128, previous.timeIv));
                if (!Arrays.equals(challenge, timed.doFinal(previous.timeSealed))) {
                    throw new IOException("time-bound persisted challenge differs");
                }
                if (!previous.sameAs(PersistedState.read(file))) {
                    throw new IOException("reopen changed the persisted fixture");
                }
                line("persistence: both original ciphertexts decrypted after new confirmation, "
                        + "challenge " + challengeId(challenge));
            }
            mPersistenceFinished = true;
            line("persistence: " + mMode + " PASSED");
            done();
        } catch (Exception e) {
            persistenceFailed(describe(e) + ": " + e.getMessage());
        }
    }

    private void persistenceFailed(String reason) {
        if (mPersistenceFinished) return;
        mPersistenceFinished = true;
        line("persistence: " + mMode + " FAILED: " + reason);
        // Only newly created fixture aliases belong to this attempt. Never alter reopened keys.
        if (mPersistenceNewAliases && mKeyStore != null) {
            for (String alias : new String[] {PERSIST_PER_OPERATION, PERSIST_TIME_BOUND}) {
                try {
                    mKeyStore.deleteEntry(alias);
                } catch (Exception e) {
                    line("persistence: cleanup failed for " + alias + ": " + describe(e));
                }
            }
        }
        done();
    }

    private static String challengeId(byte[] challenge) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(challenge);
        StringBuilder text = new StringBuilder();
        for (byte b : digest) {
            text.append("0123456789abcdef".charAt((b & 0xff) >>> 4));
            text.append("0123456789abcdef".charAt(b & 0xf));
        }
        return text.toString();
    }

    /** Bounded public test data only: no key bytes are exported or stored here. */
    static final class PersistedState {
        private static final int MAGIC = 0x47484b31;
        final byte[] challenge;
        final byte[] perIv;
        final byte[] perSealed;
        final byte[] timeIv;
        final byte[] timeSealed;

        PersistedState(byte[] challenge, byte[] perIv, byte[] perSealed,
                byte[] timeIv, byte[] timeSealed) throws IOException {
            if (challenge.length != 32 || perIv.length != 12 || perSealed.length != 48
                    || timeIv.length != 12 || timeSealed.length != 48) {
                throw new IOException("invalid AES-GCM fixture lengths");
            }
            this.challenge = challenge.clone();
            this.perIv = perIv.clone();
            this.perSealed = perSealed.clone();
            this.timeIv = timeIv.clone();
            this.timeSealed = timeSealed.clone();
        }

        static PersistedState read(File file) throws IOException {
            if (!file.isFile() || file.length() != 156) {
                throw new IOException("missing or malformed persisted fixture");
            }
            try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
                if (in.readInt() != MAGIC) throw new IOException("unknown fixture format");
                byte[] challenge = new byte[32];
                byte[] perIv = new byte[12];
                byte[] perSealed = new byte[48];
                byte[] timeIv = new byte[12];
                byte[] timeSealed = new byte[48];
                in.readFully(challenge);
                in.readFully(perIv);
                in.readFully(perSealed);
                in.readFully(timeIv);
                in.readFully(timeSealed);
                if (in.read() != -1) throw new IOException("trailing fixture data");
                return new PersistedState(challenge, perIv, perSealed, timeIv, timeSealed);
            }
        }

        void writeNew(File file) throws IOException {
            if (file.exists()) throw new IOException("refusing to replace persisted fixture");
            File temporary = File.createTempFile("hellokeys-persist-", ".tmp", file.getParentFile());
            try {
                try (FileOutputStream bytes = new FileOutputStream(temporary);
                        DataOutputStream out = new DataOutputStream(bytes)) {
                    out.writeInt(MAGIC);
                    out.write(challenge);
                    out.write(perIv);
                    out.write(perSealed);
                    out.write(timeIv);
                    out.write(timeSealed);
                    out.flush();
                    bytes.getFD().sync();
                }
                Files.move(temporary.toPath(), file.toPath());
            } finally {
                if (temporary.exists() && !temporary.delete()) {
                    line("persistence: temporary fixture cleanup failed");
                }
            }
        }

        boolean sameAs(PersistedState other) {
            return Arrays.equals(challenge, other.challenge) && Arrays.equals(perIv, other.perIv)
                    && Arrays.equals(perSealed, other.perSealed)
                    && Arrays.equals(timeIv, other.timeIv)
                    && Arrays.equals(timeSealed, other.timeSealed);
        }
    }

    /** A key without the requirement: works with or without a secure lock screen. */
    private void plain() {
        try {
            KeyGenerator generator = KeyGenerator.getInstance(
                    KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(PLAIN,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .build());
            SecretKey key = generator.generateKey();
            Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.ENCRYPT_MODE, key);
            line("plain: encrypted " + cipher.doFinal(PLAINTEXT).length + " bytes");
        } catch (Exception e) {
            line("plain: " + e);
        }
    }

    private SecretKey generate(String name, String alias, int timeoutSeconds) {
        KeyGenerator generator;
        try {
            generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,
                    "AndroidKeyStore");
        } catch (Exception e) {
            line(name + ": no generator: " + e);
            return null;
        }
        try {
            generator.init(new KeyGenParameterSpec.Builder(alias,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setUserAuthenticationRequired(true)
                    .setUserAuthenticationParameters(timeoutSeconds,
                            KeyProperties.AUTH_DEVICE_CREDENTIAL)
                    .build());
        } catch (Exception e) {
            line(name + ": init " + describe(e));
            return null;
        }
        try {
            SecretKey key = generator.generateKey();
            line(name + ": generated");
            return key;
        } catch (Exception e) {
            line(name + ": generateKey " + describe(e));
            return null;
        }
    }

    private Cipher init(String name, SecretKey key) {
        try {
            Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.ENCRYPT_MODE, key);
            line(name + ": init ok");
            return cipher;
        } catch (Exception e) {
            line(name + ": init " + describe(e));
            return null;
        }
    }

    /** Encrypts and decrypts with the time-bound key, or prints why it was refused. */
    private void useTimeBound(String when, SecretKey key) {
        try {
            Cipher encrypt = Cipher.getInstance(TRANSFORMATION);
            encrypt.init(Cipher.ENCRYPT_MODE, key);
            byte[] sealed = encrypt.doFinal(PLAINTEXT);
            Cipher decrypt = Cipher.getInstance(TRANSFORMATION);
            decrypt.init(Cipher.DECRYPT_MODE, key, encrypt.getParameters());
            boolean same = Arrays.equals(decrypt.doFinal(sealed), PLAINTEXT);
            line("time-bound: " + when + ": used, round trip " + same);
        } catch (Exception e) {
            line("time-bound: " + when + ": " + describe(e));
        }
    }

    private void prompt(SecretKey perOperation, SecretKey timeBound, Cipher cipher) {
        try {
            new BiometricPrompt.Builder(this)
                    .setTitle("Gesher test")
                    .setDescription("Confirm to use the per-operation key")
                    .setAllowedAuthenticators(Authenticators.DEVICE_CREDENTIAL)
                    .build()
                    .authenticate(new BiometricPrompt.CryptoObject(cipher),
                            new CancellationSignal(), getMainExecutor(),
                            new BiometricPrompt.AuthenticationCallback() {
                                @Override
                                public void onAuthenticationError(int code, CharSequence text) {
                                    line("prompt: crypto: error " + code);
                                    done();
                                }

                                @Override
                                public void onAuthenticationSucceeded(
                                        BiometricPrompt.AuthenticationResult result) {
                                    line("prompt: crypto: succeeded, type "
                                            + result.getAuthenticationType());
                                    authorized(perOperation, timeBound, result.getCryptoObject());
                                }

                                @Override
                                public void onAuthenticationFailed() {
                                    line("prompt: crypto: failed attempt");
                                }
                            });
            line("prompt: crypto: started");
        } catch (RuntimeException e) {
            line("prompt: crypto: throws " + e);
            done();
        }
    }

    private void authorized(SecretKey perOperation, SecretKey timeBound,
            BiometricPrompt.CryptoObject crypto) {
        try {
            Cipher cipher = crypto.getCipher();
            byte[] sealed = cipher.doFinal(PLAINTEXT);
            line("per-operation: after the prompt: encrypted " + sealed.length + " bytes");
            // The authorization was for that one operation: decrypting needs another.
            Cipher decrypt = Cipher.getInstance(TRANSFORMATION);
            decrypt.init(Cipher.DECRYPT_MODE, perOperation, cipher.getParameters());
            try {
                decrypt.doFinal(sealed);
                line("per-operation: a second operation without a prompt succeeded");
            } catch (Exception e) {
                line("per-operation: a second operation without a prompt " + describe(e));
            }
        } catch (Exception e) {
            line("per-operation: after the prompt: " + describe(e));
        }
        // On Android an operation's confirmation also counts for keys with a timeout.
        useTimeBound("after the CryptoObject confirmation", timeBound);
        line("time-bound: waiting " + (TIMEOUT_SECONDS + 2) + " s for the timeout");
        mHandler.postDelayed(() -> {
            useTimeBound("after the timeout", timeBound);
            confirm(timeBound);
        }, (TIMEOUT_SECONDS + 2) * 1000L);
    }

    private SecretKey mPendingTimeBound;

    private void confirm(SecretKey timeBound) {
        KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        Intent intent = keyguard.createConfirmDeviceCredentialIntent("Gesher test",
                "Confirm to use the time-bound key");
        if (intent == null) {
            line("confirm: createConfirmDeviceCredentialIntent null");
            done();
            return;
        }
        mPendingTimeBound = timeBound;
        startActivityForResult(intent, CONFIRM_REQUEST);
        line("confirm: started");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != CONFIRM_REQUEST) return;
        line("confirm: result " + (resultCode == RESULT_OK ? "RESULT_OK"
                : resultCode == RESULT_CANCELED ? "RESULT_CANCELED" : String.valueOf(resultCode)));
        if (mPendingTimeBound != null) {
            useTimeBound("after confirm-credential", mPendingTimeBound);
            mPendingTimeBound = null;
        }
        done();
    }

    private void done() {
        line("done");
        finish();
    }
}
