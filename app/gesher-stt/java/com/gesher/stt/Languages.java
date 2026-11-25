/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Which of Windows' recognizer languages serves a requested BCP-47 tag
 * (RecognizerIntent.EXTRA_LANGUAGE, else Locale.getDefault()). The same tag, or for a tag
 * without a region a recognizer of that language (the default recognizer's first). Another
 * region is not substituted: "en-AU" is not recognized as "en-US".
 */
final class Languages {
    private Languages() {
    }

    /** The Windows tag that serves {@code requested}, or null. */
    static String match(List<RecognizerBackend.Language> installed, String requested) {
        if (requested == null || requested.isEmpty()) return null;
        Locale locale = Locale.forLanguageTag(requested);
        String language = locale.getLanguage();
        if (language.isEmpty()) return null;
        String country = locale.getCountry();
        String wanted = country.isEmpty() ? language : language + "-" + country;
        for (RecognizerBackend.Language l : installed) {
            if (normalized(l.tag).equalsIgnoreCase(wanted)) return l.tag;
        }
        if (!country.isEmpty()) return null;
        String found = null;
        for (RecognizerBackend.Language l : installed) {
            if (!Locale.forLanguageTag(l.tag).getLanguage().equals(language)) continue;
            if (l.ofDefaultRecognizer) return l.tag;
            if (found == null) found = l.tag;
        }
        return found;
    }

    /** "en-US" for "en-US", and Java's current codes for legacy ones ("iw" is "he"). */
    private static String normalized(String tag) {
        Locale l = Locale.forLanguageTag(tag);
        return l.getCountry().isEmpty() ? l.getLanguage() : l.getLanguage() + "-" + l.getCountry();
    }

    /** The installed tags, for RecognitionSupport. */
    static List<String> tags(List<RecognizerBackend.Language> installed) {
        List<String> out = new ArrayList<>(installed.size());
        for (RecognizerBackend.Language l : installed) out.add(l.tag);
        return out;
    }
}
