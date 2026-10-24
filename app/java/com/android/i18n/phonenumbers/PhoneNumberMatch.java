/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.android.i18n.phonenumbers;

/**
 * A placeholder for libphonenumber's PhoneNumberMatch, for {@link PhoneNumberUtil}'s
 * signatures: the placeholder finds no numbers, so none is ever made.
 */
public final class PhoneNumberMatch {
    private final int mStart;
    private final String mRaw;

    PhoneNumberMatch(int start, String raw) {
        mStart = start;
        mRaw = raw;
    }

    public int start() {
        return mStart;
    }

    public int end() {
        return mStart + mRaw.length();
    }

    public String rawString() {
        return mRaw;
    }
}
