/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.hellocred;

import android.app.Activity;
import android.app.KeyguardManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricManager.Authenticators;
import android.hardware.biometrics.BiometricPrompt;
import android.hardware.biometrics.IAuthService;
import android.hardware.biometrics.IBiometricServiceReceiver;
import android.hardware.biometrics.PromptInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.util.Log;
import android.widget.TextView;

/**
 * Asks Android for the device credential the way apps do and prints every answer.
 *
 * <p>{@code $GESHER_HELLOCRED_MODE}:
 * <ul>
 * <li>{@code answers} (CI): prints the answers, and starts the credential prompt and the
 *     confirm-credential intent only on a device that is not secure, where each must end
 *     at once with Android's unavailable result. On a secure device it shows no prompt.
 * <li>unset, or {@code prompt} (by hand): also on a secure device; the person at the PC
 *     verifies, cancels, switches to another window or ends the app.
 * <li>{@code app-cancel}: as {@code prompt}, and the app cancels its BiometricPrompt after
 *     five seconds.
 * </ul>
 */
public class MainActivity extends Activity {
    private static final int CONFIRM_REQUEST = 7;
    private static final String ACTION_CONFIRM = "android.app.action.CONFIRM_DEVICE_CREDENTIAL";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mMode;
    private boolean mSecure;
    private boolean mManual;
    private boolean mManualStarted;

    private static void line(String message) {
        System.out.println("hellocred: " + message);
        System.out.flush();
        Log.i("hellocred", message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) return;
        String mode = System.getenv("GESHER_HELLOCRED_MODE");
        mMode = mode == null || mode.isEmpty() ? "prompt" : mode;
        mManual = getIntent().getBooleanExtra("manual-hello", false);
        if (mManual) {
            if (getIntent().getBooleanExtra("app-cancel", false)) mMode = "app-cancel";
            else if (getIntent().getBooleanExtra("owner-lost", false)) mMode = "owner-lost";
            else if (getIntent().getBooleanExtra("user-cancel", false)) mMode = "user-cancel";
            else mMode = "verify";
            TextView view = new TextView(this);
            view.setText("Gesher Windows Hello verification");
            setContentView(view);
        }
        line("mode: " + mMode);
        if (mManual) line("probe-process: " + android.os.Process.myPid());
        line("feature: secure_lock_screen "
                + getPackageManager().hasSystemFeature(PackageManager.FEATURE_SECURE_LOCK_SCREEN));
        KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        mSecure = keyguard.isDeviceSecure();
        line("keyguard: isDeviceSecure " + mSecure + ", isKeyguardSecure "
                + keyguard.isKeyguardSecure() + ", isDeviceLocked " + keyguard.isDeviceLocked()
                + ", isKeyguardLocked " + keyguard.isKeyguardLocked());
        BiometricManager biometrics = getSystemService(BiometricManager.class);
        if (biometrics == null) {
            line("biometrics: no BiometricManager");
        } else {
            line("biometrics: canAuthenticate credential "
                    + can(biometrics, Authenticators.DEVICE_CREDENTIAL)
                    + ", strong " + can(biometrics, Authenticators.BIOMETRIC_STRONG)
                    + ", weak " + can(biometrics, Authenticators.BIOMETRIC_WEAK)
                    + ", strong|credential " + can(biometrics,
                            Authenticators.BIOMETRIC_STRONG | Authenticators.DEVICE_CREDENTIAL)
                    + ", weak|credential " + can(biometrics,
                            Authenticators.BIOMETRIC_WEAK | Authenticators.DEVICE_CREDENTIAL));
        }
        Intent created = keyguard.createConfirmDeviceCredentialIntent("Gesher test",
                "Confirm it is you");
        line("keyguard: createConfirmDeviceCredentialIntent "
                + (created == null ? "null" : created.getAction() + " for " + created.getPackage()));
        if (!mManual) mHandler.post(this::biometricOnly);
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (mManual && focused && !mManualStarted) {
            mManualStarted = true;
            mHandler.post(this::operationBound);
        }
    }

    /** There is no supported key handle to obtain from AndroidKeyStore today. Exercise
     * the actual Binder refusal for a nonzero operation ID without creating a pretend key. */
    private void operationBound() {
        IAuthService auth = IAuthService.Stub.asInterface(ServiceManager.getService("auth"));
        PromptInfo info = new PromptInfo();
        info.setTitle("Gesher operation-bound verification");
        info.setAuthenticators(Authenticators.BIOMETRIC_STRONG | Authenticators.DEVICE_CREDENTIAL);
        try {
            if (auth == null) throw new IllegalStateException("No auth service");
            auth.authenticate(new Binder(), 1L, UserHandle.myUserId(),
                    new IBiometricServiceReceiver.Stub() {
                @Override public void onAuthenticationSucceeded(int type) {
                    line("operation-bound: unexpected success");
                    mHandler.post(MainActivity.this::biometricOnly);
                }

                @Override public void onAuthenticationFailed() { }

                @Override public void onError(int modality, int errorCode, int vendorCode) {
                    line("operation-bound: error " + errorCode);
                    mHandler.post(MainActivity.this::biometricOnly);
                }

                @Override public void onAcquired(int acquireInfo, String message) { }
                @Override public void onDialogDismissed(int reason) { }
                @Override public void onSystemEvent(int event) { }
            }, getPackageName(), info);
        } catch (RuntimeException | RemoteException failure) {
            line("operation-bound: could not invoke auth API");
            mHandler.post(this::biometricOnly);
        }
    }

    private static String can(BiometricManager biometrics, int authenticators) {
        try {
            return String.valueOf(biometrics.canAuthenticate(authenticators));
        } catch (RuntimeException e) {
            return e.getClass().getSimpleName();
        }
    }

    private BiometricPrompt.AuthenticationCallback callback(String name, Runnable next) {
        return new BiometricPrompt.AuthenticationCallback() {
            @Override
            public void onAuthenticationError(int errorCode, CharSequence errString) {
                line("prompt: " + name + ": error " + errorCode + ", window focus "
                        + hasWindowFocus());
                mHandler.post(next);
            }

            @Override
            public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
                line("prompt: " + name + ": succeeded, type " + result.getAuthenticationType()
                        + ", crypto " + result.getCryptoObject());
                mHandler.post(next);
            }

            @Override
            public void onAuthenticationFailed() {
                line("prompt: " + name + ": failed attempt");
            }
        };
    }

    /** A biometric-only prompt: no sensor exists, on a secure device or not. */
    private void biometricOnly() {
        try {
            new BiometricPrompt.Builder(this)
                    .setTitle("Gesher test")
                    .setAllowedAuthenticators(Authenticators.BIOMETRIC_STRONG)
                    .setNegativeButton("Cancel", getMainExecutor(),
                            (dialog, which) -> line("prompt: strong: negative button"))
                    .build()
                    .authenticate(new CancellationSignal(), getMainExecutor(),
                            callback("strong", this::credential));
        } catch (RuntimeException e) {
            line("prompt: strong: throws " + e);
            mHandler.post(this::credential);
        }
    }

    /** The device credential, with biometric bits that fall to it. */
    private void credential() {
        if (mSecure && "answers".equals(mMode)) {
            line("prompt: credential: not started (the device is secure and nobody is here to answer)");
            line("done");
            finish();
            return;
        }
        try {
            CancellationSignal cancel = new CancellationSignal();
            new BiometricPrompt.Builder(this)
                    .setTitle("Gesher test")
                    .setSubtitle("BiometricPrompt")
                    .setDescription("Confirm it is you")
                    .setAllowedAuthenticators(
                            Authenticators.BIOMETRIC_STRONG | Authenticators.DEVICE_CREDENTIAL)
                    .build()
                    .authenticate(cancel, getMainExecutor(), callback("credential", this::afterCredential));
            line("prompt: credential: started");
            if ("app-cancel".equals(mMode)) {
                mHandler.postDelayed(() -> {
                    line("prompt: credential: the app cancels");
                    cancel.cancel();
                }, 5000);
            }
        } catch (RuntimeException e) {
            line("prompt: credential: throws " + e);
            mHandler.post(this::afterCredential);
        }
    }

    private void afterCredential() {
        if (mManual && ("app-cancel".equals(mMode) || "owner-lost".equals(mMode))) {
            line("done");
            finish();
        } else {
            confirm();
        }
    }

    /** The confirm-credential activity, started as KeyguardManager's intent is. */
    private void confirm() {
        KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        Intent intent = keyguard.createConfirmDeviceCredentialIntent("Gesher test",
                "Confirm it is you (confirm-credential intent)");
        if (intent == null) {
            // What an app that skips the null check sends.
            intent = new Intent(ACTION_CONFIRM).setPackage("com.android.settings");
            intent.putExtra(KeyguardManager.EXTRA_TITLE, "Gesher test");
            line("confirm: starting the action directly (the device is not secure)");
        }
        try {
            startActivityForResult(intent, CONFIRM_REQUEST);
            line("confirm: started");
        } catch (ActivityNotFoundException e) {
            line("confirm: ActivityNotFoundException");
            frp();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != CONFIRM_REQUEST) return;
        line("confirm: result " + (resultCode == RESULT_OK ? "RESULT_OK"
                : resultCode == RESULT_CANCELED ? "RESULT_CANCELED" : String.valueOf(resultCode)));
        frp();
    }

    /** The factory-reset variant has no handler: Gesher has no factory reset protection. */
    private void frp() {
        try {
            startActivityForResult(new Intent("android.app.action.CONFIRM_FRP_CREDENTIAL")
                    .setPackage("com.android.settings"), CONFIRM_REQUEST + 1);
            line("confirm: FRP started");
        } catch (ActivityNotFoundException e) {
            line("confirm: FRP ActivityNotFoundException");
        }
        line("done");
        finish();
    }
}
