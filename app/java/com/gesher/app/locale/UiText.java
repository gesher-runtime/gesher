/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.locale;

/** Gesher's own UI always follows the host, independently of an Android app's locale. */
public final class UiText {
    private UiText() {}

    public static String language() {
        return UiMessages.language(SystemLocale.locales().toLanguageTags());
    }

    public static String text(String key, Object... args) {
        return UiMessages.text(key, SystemLocale.locales().toLanguageTags(), args);
    }
}
