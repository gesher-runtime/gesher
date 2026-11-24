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
import android.util.Log;

import java.security.KeyStore;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

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
