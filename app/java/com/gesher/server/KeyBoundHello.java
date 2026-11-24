/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * The credential confirmation that also releases keystore keys with a timeout (ADR 0095).
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
        Chain chain = new Chain(request, completion);
        chain.start();
        return chain;
    }

    /** The verification in progress: the key's, then possibly the verifier's. */
    private final class Chain implements Verification {
        private final Request mRequest;
        private final Completion mCompletion;
        private Verification mCurrent;
        private boolean mCanceled;

        Chain(Request request, Completion completion) {
            mRequest = request;
            mCompletion = completion;
        }

        synchronized void start() {
            // Publish the key's handle before its callback can start the verifier.
            mCurrent = mKeys.authorize(mRequest, 0, this::keyFinished);
        }

        private void keyFinished(int result) {
            synchronized (this) {
                if (mCanceled) {
                    result = CANCELED;
                } else if (result == KeystoreAuthorization.NO_BINDING) {
                    mCurrent = mVerifier.verify(mRequest, mCompletion);
                    return;
                }
            }
            mCompletion.onResult(result);
        }

        @Override
        public void cancel() {
            Verification current;
            synchronized (this) {
                if (mCanceled) return;
                mCanceled = true;
                current = mCurrent;
            }
            current.cancel();
        }
    }
}
