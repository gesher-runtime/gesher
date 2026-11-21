/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.os.Bundle;
import android.os.Process;
import android.service.textservice.SpellCheckerService;
import android.view.textservice.SuggestionsInfo;
import android.view.textservice.TextInfo;

/** Test-only Android spell checker, running in a separate process from its client. */
public final class TestSpellChecker extends SpellCheckerService {
    @Override public Session createSession() {
        return new Session() {
            private boolean mEnglish;
            private boolean mCanceled;

            @Override public void onCreate() {
                mEnglish = "en_US".equals(getLocale());
                if (!mEnglish && !"fr_FR".equals(getLocale())) throw new AssertionError("Unknown fixture locale");
                Bundle extras = getBundle();
                if (extras == null || extras.getInt("client-pid", -1) == -1
                        || extras.getInt("client-pid") == Process.myPid()) {
                    throw new AssertionError("Fixture must cross a real process boundary");
                }
            }

            @Override public SuggestionsInfo onGetSuggestions(TextInfo text, int limit) {
                if (limit < 1) throw new AssertionError("Suggestion limit was not forwarded");
                String word = text.getText();
                if (mEnglish && word.equals("teh")) return correction("the");
                if (!mEnglish && word.equals("bonjor") && mCanceled) return correction("bonjour");
                return new SuggestionsInfo(SuggestionsInfo.RESULT_ATTR_IN_THE_DICTIONARY, new String[0]);
            }

            @Override public void onCancel() { mCanceled = true; }
            @Override public void onClose() { System.out.println("fixture spell checker closed " + getLocale()); }
        };
    }

    private static SuggestionsInfo correction(String suggestion) {
        return new SuggestionsInfo(SuggestionsInfo.RESULT_ATTR_LOOKS_LIKE_TYPO,
                new String[] {suggestion});
    }
}
