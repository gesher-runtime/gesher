/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * What {@link KeystoreAuthorization} asks of Windows Hello's key store (ADR 0092):
 * {@code Windows.Security.Credentials.KeyCredentialManager} and {@code KeyCredential}. A
 * key lives in Windows, under a name, for the signed-in Windows user; every signature needs
 * the user's Windows Hello gesture (PIN or a biometric, Windows does not say which). The
 * status values are Windows' own {@code KeyCredentialStatus}.
 *
 * <p>The only production implementation is {@link WindowsHelloKey}. Nothing here can
 * produce a signature without Windows: there is no software fallback.
 */
public interface HelloKeyBackend {
    // KeyCredentialStatus.
    int SUCCESS = 0;
    int UNKNOWN_ERROR = 1;
    int NOT_FOUND = 2;
    int USER_CANCELED = 3;
    int USER_PREFERS_PASSWORD = 4;
    int CREDENTIAL_ALREADY_EXISTS = 5;
    int SECURITY_DEVICE_LOCKED = 6;
    /** Not a Windows value: the call could not start or failed with an HRESULT. */
    int FAILED = -1;
    /** Not a Windows value: the requesting process does not own the foreground window. */
    int NOT_FOREGROUND = -2;

    /** KeyCredentialManager.IsSupportedAsync: the user has Windows Hello set up. */
    boolean isSupported();

    /**
     * KeyCredentialManager.RequestCreateAsync(name, FailIfExists), shown over the foreground
     * window of {@code pid}. Blocks until the user answers. SUCCESS carries the key's public
     * key (X.509 SubjectPublicKeyInfo).
     */
    Result create(String name, int pid);

    /**
     * KeyCredentialManager.OpenAsync and the key's public key (SubjectPublicKeyInfo). Shows
     * no UI. NOT_FOUND when Windows has no such key, as after a Windows Hello reset.
     */
    Result open(String name);

    /**
     * KeyCredential.RequestSignAsync over {@code data}, shown over the foreground window of
     * {@code pid}. {@code completion} is called exactly once, on any thread, never before
     * this returns; SUCCESS carries the signature.
     */
    Operation sign(String name, int pid, byte[] data, Completion completion);

    /** KeyCredentialManager.DeleteAsync; a status value. */
    int delete(String name);

    /** A status and, with SUCCESS, the bytes Windows returned. */
    final class Result {
        public final int status;
        public final byte[] data;

        public Result(int status, byte[] data) {
            this.status = status;
            this.data = data;
        }
    }

    interface Completion {
        void onResult(Result result);
    }

    interface Operation {
        /** Asks Windows to close the prompt; the completion still reports its result. */
        void cancel();
    }
}
