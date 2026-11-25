/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import com.gesher.server.WindowsSpeech;

import java.util.ArrayList;
import java.util.List;

/**
 * Windows' speech synthesis ({@link WindowsSpeech}, on Gesher's boot class path): the
 * engine's production backend.
 */
final class WindowsSpeechBackend implements SpeechBackend {
    @Override
    public List<BackendVoice> voices() {
        List<BackendVoice> voices = new ArrayList<>();
        for (WindowsSpeech.Voice v : WindowsSpeech.voices()) {
            voices.add(new BackendVoice(v.id, v.displayName, v.language, v.female, v.isDefault));
        }
        return voices;
    }

    @Override
    public Synthesis start(String text, String voiceId, double rate, double pitch, boolean words)
            throws StartFailure {
        final WindowsSpeech.Synthesis s;
        try {
            s = WindowsSpeech.start(text, voiceId, rate, pitch, words);
        } catch (WindowsSpeech.SpeechException e) {
            throw new StartFailure(e.reason, e.getMessage());
        }
        return new Synthesis() {
            @Override public int await(long timeoutMs) { return s.await(timeoutMs); }
            @Override public byte[] audio() { return s.audio(); }
            @Override public long[] words() { return s.words(); }
            @Override public void cancel() { s.cancel(); }
            @Override public void release() { s.release(); }
        };
    }
}
