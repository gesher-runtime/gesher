/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * Keystore operations that wait for the user (ADR 0095): what keystore2 is to
 * BiometricService for {@code BiometricPrompt.CryptoObject}. Implemented by
 * {@link KeystoreAuthorization}.
 */
public interface KeystoreOperations {
    /** Whether {@code operationId} is an operation of {@code uid} not yet authorized. */
    boolean isPending(int uid, long operationId);

    /**
     * Asks the user to authorize it, as {@link WindowsHelloBackend#verify}: the completion
     * gets VERIFIED only when the operation got its key, exactly once and never before
     * this returns.
     */
    WindowsHelloBackend.Verification authorize(WindowsHelloBackend.Request request,
            long operationId, WindowsHelloBackend.Completion completion);

    /**
     * The confirmation to use for a credential prompt without an operation: {@code verifier}
     * itself, or one that also releases keys with a timeout ({@link KeyBoundHello}).
     */
    default WindowsHelloBackend confirming(WindowsHelloBackend verifier) {
        return verifier;
    }
}
