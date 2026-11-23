/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * What {@link AuthService} asks of Windows Hello (ADR 0091): whether the signed-in user can
 * confirm a credential, and that confirmation. The values are Windows' own
 * ({@code Windows.Security.Credentials.UI.UserConsentVerifierAvailability} and
 * {@code UserConsentVerificationResult}); the native implementation is specified in
 * docs/research/windows-hello-native-spec.md.
 *
 * <p>A verification proves only that the user confirmed a credential of this Windows
 * account (PIN, or a Windows Hello biometric that Windows does not name). It binds no key.
 * The service therefore uses it for {@code DEVICE_CREDENTIAL} only, never as a biometric
 * authenticator, and this interface deliberately reports no biometric kinds.
 */
public interface WindowsHelloBackend {
    // UserConsentVerifierAvailability.
    int AVAILABLE = 0;
    int DEVICE_NOT_PRESENT = 1;
    int NOT_CONFIGURED_FOR_USER = 2;
    int DISABLED_BY_POLICY = 3;
    int DEVICE_BUSY = 4;

    // UserConsentVerificationResult; DEVICE_NOT_PRESENT..DEVICE_BUSY as above.
    int VERIFIED = 0;
    int RETRIES_EXHAUSTED = 5;
    int CANCELED = 6;
    /** Not a Windows value: the verification could not start or failed with an HRESULT. */
    int FAILED = -1;

    /** The user's current availability (CheckAvailabilityAsync), one of the values above. */
    int availability();

    /**
     * Starts a verification owned by the requesting app's window. {@code completion} is
     * called exactly once, on any thread, with a result value above (CANCELED after
     * {@link Verification#cancel}). Must not call {@code completion} before returning.
     */
    Verification verify(Request request, Completion completion);

    /** The prompt's owner and text. */
    final class Request {
        public final int uid;
        public final int pid;
        public final String packageName;
        /** The message Windows shows: the app's title, subtitle and description. */
        public final String message;

        public Request(int uid, int pid, String packageName, String message) {
            this.uid = uid;
            this.pid = pid;
            this.packageName = packageName;
            this.message = message;
        }
    }

    interface Completion {
        void onResult(int result);
    }

    interface Verification {
        /** Asks Windows to close the prompt; the completion still reports its result. */
        void cancel();
    }
}
