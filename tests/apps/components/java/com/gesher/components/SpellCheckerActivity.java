/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.provider.Settings;
import android.view.textservice.SentenceSuggestionsInfo;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSession;
import android.view.textservice.SuggestionsInfo;
import android.view.textservice.TextInfo;
import android.view.textservice.TextServicesManager;

import java.util.Locale;

public final class SpellCheckerActivity extends Activity {
    private SpellCheckerSession mEnglish;
    private SpellCheckerSession mFrench;

    private static void check(boolean yes, String message) {
        if (!yes) throw new AssertionError(message);
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextServicesManager manager = getSystemService(TextServicesManager.class);
        SpellCheckerInfo checker = manager.getCurrentSpellCheckerInfo();
        if (getIntent().getBooleanExtra("spell-checker-unselected", false)) {
            check(manager.isSpellCheckerEnabled() && checker == null,
                    "Side-loaded checker must never be selected automatically");
            check(manager.getEnabledSpellCheckerInfos().size() == 1, "Valid checker should be discoverable");
            System.out.println("spell checker contract: unselected APK remains inactive");
            finish();
            return;
        }
        check(checker != null && checker.getId().equals("com.gesher.components/.TestSpellChecker"),
                "Current checker does not match installed and approved component");
        check(manager.isSpellCheckerEnabled() && checker.getSubtypeCount() == 2, "Metadata lost");
        check(manager.getCurrentSpellCheckerSubtype(false).hashCode() == 1002, "Selected language lost");
        try {
            Settings.Secure.putString(getContentResolver(), Settings.Secure.SELECTED_SPELL_CHECKER, "");
            throw new AssertionError("Ordinary client changed selection");
        } catch (SecurityException expected) { }

        Bundle extras = new Bundle();
        extras.putInt("client-pid", Process.myPid());
        mEnglish = manager.newSpellCheckerSession(extras, Locale.US, listener(false), false);
        mFrench = manager.newSpellCheckerSession(extras, null, listener(true), true);
        check(mEnglish != null && mFrench != null, "Both locale sessions must be available");
        mEnglish.getSuggestions(new TextInfo("teh", 17, 42), 3);
        getWindow().getDecorView().postDelayed(() -> {
            if (!isFinishing()) throw new AssertionError("Timed out waiting for checker results");
        }, 30_000);
    }

    private SpellCheckerSession.SpellCheckerSessionListener listener(boolean french) {
        return new SpellCheckerSession.SpellCheckerSessionListener() {
            @Override public void onGetSuggestions(SuggestionsInfo[] result) {
                check(!french && result.length == 1 && result[0].getSuggestionsCount() == 1,
                        "Expected a single English correction");
                check(result[0].getCookie() == 17 && result[0].getSequence() == 42
                        && "the".equals(result[0].getSuggestionAt(0)), "Word metadata or correction lost");
                mEnglish.close();
                mEnglish = null;
                // The first sentence result synchronizes session creation before cancel.
                mFrench.getSentenceSuggestions(new TextInfo[] {new TextInfo("le bonjor", 19, 43)}, 3);
            }

            @Override public void onGetSentenceSuggestions(SentenceSuggestionsInfo[] result) {
                check(french && result.length == 1 && result[0].getSuggestionsCount() == 2,
                        "Expected two French sentence tokens");
                SuggestionsInfo correction = result[0].getSuggestionsInfoAt(1);
                if (correction.getSuggestionsCount() == 0) {
                    mFrench.cancel();
                    mFrench.getSentenceSuggestions(new TextInfo[] {new TextInfo("le bonjor", 19, 43)}, 3);
                    return;
                }
                check(result[0].getOffsetAt(1) == 3 && result[0].getLengthAt(1) == 6
                        && correction.getCookie() == 19 && correction.getSequence() == 43
                        && "bonjour".equals(correction.getSuggestionAt(0)), "Sentence metadata or correction lost");
                mFrench.close();
                mFrench = null;
                System.out.println("spell checker contract: distinct process, words, sentence, locale, cancel and close passed");
                finish();
            }
        };
    }

    @Override public void onDestroy() {
        if (mEnglish != null) mEnglish.close();
        if (mFrench != null) mFrench.close();
        super.onDestroy();
    }
}
