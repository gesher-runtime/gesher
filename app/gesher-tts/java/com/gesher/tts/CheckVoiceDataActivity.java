/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;

import java.util.ArrayList;

/**
 * TextToSpeech.Engine.ACTION_CHECK_TTS_DATA: answers, without UI, which voices are
 * installed: CHECK_VOICE_DATA_PASS with EXTRA_AVAILABLE_VOICES ("heb-ISR", ...) when
 * Windows has a voice (of the languages EXTRA_CHECK_VOICE_DATA_FOR asks about, if it
 * does), else CHECK_VOICE_DATA_FAIL. EXTRA_UNAVAILABLE_VOICES ("ones that user can
 * install") is empty: this engine installs no voice data; Windows installs its voices.
 */
public final class CheckVoiceDataActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ArrayList<String> requested = getIntent().getStringArrayListExtra(
                TextToSpeech.Engine.EXTRA_CHECK_VOICE_DATA_FOR);
        ArrayList<String> available = VoiceCatalog.of(new WindowsSpeechBackend().voices())
                .availableVoices(requested);
        setResult(resultCode(available), result(available));
        finish();
    }

    static int resultCode(ArrayList<String> available) {
        return available.isEmpty() ? TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL
                : TextToSpeech.Engine.CHECK_VOICE_DATA_PASS;
    }

    static Intent result(ArrayList<String> available) {
        Intent data = new Intent();
        data.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, available);
        data.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES,
                new ArrayList<String>());
        return data;
    }
}
