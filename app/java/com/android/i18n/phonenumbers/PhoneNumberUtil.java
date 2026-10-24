/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.android.i18n.phonenumbers;

import java.util.Collections;

/**
 * A placeholder for libphonenumber's PhoneNumberUtil (repackaged as
 * com.android.i18n.phonenumbers on Android), which Gesher's framework.jar leaves out:
 * {@code Linkify.addLinks(..., PHONE_NUMBERS | ALL)} asks it for the phone numbers in a text
 * (docs/e2e.md: NoNonsense Notes linkifies every note title). It finds none, so no phone
 * links are made. When framework.jar brings the real class, it wins.
 */
public class PhoneNumberUtil {
    /** As PhoneNumberUtil.Leniency. */
    public enum Leniency { POSSIBLE, VALID, STRICT_GROUPING, EXACT_GROUPING }

    private static final PhoneNumberUtil INSTANCE = new PhoneNumberUtil();

    PhoneNumberUtil() {
    }

    public static PhoneNumberUtil getInstance() {
        return INSTANCE;
    }

    /** No phone numbers. */
    public Iterable<PhoneNumberMatch> findNumbers(CharSequence text, String defaultRegion) {
        return Collections.emptyList();
    }

    /** No phone numbers. */
    public Iterable<PhoneNumberMatch> findNumbers(CharSequence text, String defaultRegion,
            Leniency leniency, long maxTries) {
        return Collections.emptyList();
    }
}
