/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * The credential confirmation that also releases keystore keys with a timeout (ADR 0092).
 * On a device a confirmed credential adds an auth token to keystore, which keys with a
 * user-authentication timeout then accept. Here such keys are released only by a
 * signature of the keystore binding's Windows Hello key, so while the binding has such
 * keys the confirmation is that signature: the same Windows Hello gesture, once. Without
 * such keys, or when the binding turns out to be gone before Windows asked anything, the
 * confirmation is the plain verifier's.
 */
final class KeyBoundHello implements WindowsHelloBackend {
    private final WindowsHelloBackend mVerifier;
    private final KeystoreAuthorization mKeys;

    KeyBoundHello(WindowsHelloBackend verifier, KeystoreAuthorization keys) {
        mVerifier = verifier;
        mKeys = keys;
    }

    @Override
    public int availability() {
        return mVerifier.availability();
    }

    @Override
    public Verification verify(Request request, Completion completion) {
        if (!mKeys.confirmsWithKey()) return mVerifier.verify(request, completion);
        Chain chain = new Chain();
        chain.set(mKeys.authorize(request, 0, result -> {
            if (result != KeystoreAuthorization.NO_BINDING) {
                completion.onResult(result);
            } else if (!chain.set(mVerifier.verify(request, completion))) {
                // Canceled in between: the verifier's completion reports it.
            }
        }));
        return chain;
    }

    /** The verification in progress: the key's, then possibly the verifier's. */
    private static final class Chain implements Verification {
        private Verification mCurrent;
        private boolean mCanceled;

        /** False when already canceled: {@code next} is canceled at once. */
        synchronized boolean set(Verification next) {
            mCurrent = next;
            if (mCanceled) next.cancel();
            return !mCanceled;
        }

        @Override
        public synchronized void cancel() {
            mCanceled = true;
            if (mCurrent != null) mCurrent.cancel();
        }
    }
}
