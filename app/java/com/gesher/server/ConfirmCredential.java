/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.Activity;
import android.content.Intent;
import android.hardware.biometrics.BiometricManager.Authenticators;

import java.util.Set;
import java.util.function.IntConsumer;

/**
 * {@code ACTION_CONFIRM_DEVICE_CREDENTIAL}, the intent of
 * {@code KeyguardManager.createConfirmDeviceCredentialIntent} (ADR 0094, phase 2b). On
 * Android, Settings' ConfirmDeviceCredentialActivity answers it: a BiometricPrompt for
 * {@code DEVICE_CREDENTIAL | BIOMETRIC_WEAK} with the intent's title and description, which
 * finishes with {@code RESULT_OK} when the user confirmed and {@code RESULT_CANCELED}
 * otherwise. Gesher has no Settings package; the activity manager answers the intent with
 * this, over the same {@link AuthService} confirmation that BiometricPrompt gets.
 *
 * <p>{@code RESULT_OK} is reported only after Windows reported the user as verified. That
 * includes the one place where Settings answers {@code RESULT_OK} without asking: a device
 * with no screen lock at all ("No pin/pattern/pass set"). Gesher cannot say that of a
 * Windows account (it may well have a password that Windows Hello does not cover), so with
 * no credential to confirm the answer is {@code RESULT_CANCELED}.
 *
 * <p>The factory-reset, repair-mode, remote-device and per-user variants of the action are
 * not handled: they stay unresolved, as their own features are absent.
 */
final class ConfirmCredential {
    static final String ACTION = "android.app.action.CONFIRM_DEVICE_CREDENTIAL";
    static final String EXTRA_TITLE = "android.app.extra.TITLE";
    static final String EXTRA_DESCRIPTION = "android.app.extra.DESCRIPTION";
    /** ConfirmDeviceCredentialActivity.BIOMETRIC_PROMPT_AUTHENTICATORS. */
    static final String EXTRA_AUTHENTICATORS = "biometric_prompt_authenticators";
    private static final String SETTINGS = "com.android.settings";

    private ConfirmCredential() {
    }

    /**
     * Whether {@code intent} is the confirm-credential request as apps send it: the action,
     * for no package or the Settings package KeyguardManager names when nothing resolves it,
     * with no component, data or type, and at most CATEGORY_DEFAULT.
     */
    static boolean handles(Intent intent, String resolvedType) {
        if (intent == null || !ACTION.equals(intent.getAction())) return false;
        if (intent.getComponent() != null || intent.getSelector() != null
                || intent.getData() != null || intent.getType() != null || resolvedType != null) {
            return false;
        }
        String pkg = intent.getPackage();
        if (pkg != null && !SETTINGS.equals(pkg)) return false;
        Set<String> categories = intent.getCategories();
        return categories == null || categories.isEmpty()
                || categories.size() == 1 && categories.contains(Intent.CATEGORY_DEFAULT);
    }

    /**
     * Runs the request for the process {@code pid} of {@code packageName}. {@code completion}
     * gets the activity result exactly once, on any thread: {@code RESULT_OK} only for a
     * confirmed credential. The returned confirmation cancels it (null when it has ended
     * already).
     */
    static AuthService.Confirmation request(AuthService auth, Intent intent, int uid, int pid,
            String packageName, IntConsumer completion) {
        String title;
        CharSequence description;
        int authenticators;
        try {
            // Settings reads the title as a String and the description as a CharSequence.
            title = intent.getStringExtra(EXTRA_TITLE);
            description = intent.getCharSequenceExtra(EXTRA_DESCRIPTION);
            authenticators = intent.getIntExtra(EXTRA_AUTHENTICATORS,
                    Authenticators.DEVICE_CREDENTIAL | Authenticators.BIOMETRIC_WEAK);
        } catch (RuntimeException unreadable) {
            // Extras this process cannot read end Settings' activity too.
            SystemServer.log("confirm-credential: unreadable extras from " + packageName + ": "
                    + unreadable);
            completion.accept(Activity.RESULT_CANCELED);
            return null;
        }
        if (auth == null || (authenticators & Authenticators.DEVICE_CREDENTIAL) == 0) {
            // Without the credential Settings' prompt has only biometrics to offer, and there
            // are none: it ends without a result ("device credential not requested").
            completion.accept(Activity.RESULT_CANCELED);
            return null;
        }
        StringBuilder message = new StringBuilder();
        if (title != null && !title.isEmpty()) message.append(title);
        if (description != null && description.length() > 0) {
            if (message.length() > 0) message.append('\n');
            message.append(description);
        }
        return auth.confirmDeviceCredential(uid, pid, packageName, message.toString(),
                confirmed -> completion.accept(
                        confirmed ? Activity.RESULT_OK : Activity.RESULT_CANCELED));
    }
}
