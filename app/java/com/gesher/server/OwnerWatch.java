/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * Decides when a Windows Hello prompt's owner has left the foreground (ADR 0091, phase 2b).
 * SystemUI closes a BiometricPrompt as a user cancel when its client is no longer the top
 * app (AuthController.cancelIfOwnerIsNotInForeground); {@link WindowsHello} samples where
 * Windows' foreground window is relative to the prompt's owner and asks this.
 *
 * <p>While Windows shows its prompt the foreground window is the prompt, not the app. The
 * prompt is recognised as a window of another process that the app's window owns. Until
 * such a window has been seen once, another process's window in the foreground is not
 * taken as the app leaving: were Windows to host its prompt without that ownership, every
 * verification would otherwise be canceled as soon as its prompt appeared. The owner's
 * window being destroyed (its process exited) always ends the watch.
 */
final class OwnerWatch {
    /** The foreground window belongs to the owner's process. */
    static final int OWNER = 0;
    /** Another process's window that the owner window owns: Windows' prompt. */
    static final int OWNED = 1;
    /** A window that is neither. */
    static final int OTHER = 2;
    /** The owner window no longer exists or changed its process. */
    static final int GONE = 3;
    /** No foreground window right now (Windows is between activations), or unknown. */
    static final int NONE = 4;

    /** Consecutive OTHER samples that mean the owner left, so a passing activation does not. */
    static final int LEFT_SAMPLES = 2;

    private boolean mPromptSeen;
    private int mAway;

    /** Whether the prompt has been seen as a window owned by the app. */
    boolean promptSeen() {
        return mPromptSeen;
    }

    /** Takes one sample; true when the verification must be canceled. */
    boolean sample(int relation) {
        switch (relation) {
            case GONE:
                return true;
            case OWNED:
                mPromptSeen = true;
                mAway = 0;
                return false;
            case OWNER:
                mAway = 0;
                return false;
            case OTHER:
                if (!mPromptSeen) return false;
                return ++mAway >= LEFT_SAMPLES;
            default:
                return false;
        }
    }
}
