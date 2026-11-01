/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.tasks;

import java.util.ArrayList;
import java.util.List;

public class CancellationTokenSource {
    private final TokenImpl mToken = new TokenImpl();

    public CancellationToken getToken() {
        return mToken;
    }

    public void cancel() {
        mToken.cancel();
    }

    private static final class TokenImpl extends CancellationToken {
        private volatile boolean mCanceled;
        private final List<OnTokenCanceledListener> mListeners = new ArrayList<>();

        @Override
        public boolean isCancellationRequested() {
            return mCanceled;
        }

        @Override
        public CancellationToken onCanceledRequested(OnTokenCanceledListener listener) {
            boolean fire = false;
            synchronized (mListeners) {
                if (mCanceled) {
                    fire = true;
                } else {
                    mListeners.add(listener);
                }
            }
            if (fire && listener != null) {
                listener.onCanceled();
            }
            return this;
        }

        void cancel() {
            List<OnTokenCanceledListener> toFire = null;
            synchronized (mListeners) {
                if (!mCanceled) {
                    mCanceled = true;
                    toFire = new ArrayList<>(mListeners);
                    mListeners.clear();
                }
            }
            if (toFire != null) {
                for (OnTokenCanceledListener l : toFire) {
                    l.onCanceled();
                }
            }
        }
    }
}
