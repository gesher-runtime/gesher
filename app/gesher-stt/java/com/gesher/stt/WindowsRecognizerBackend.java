/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import com.gesher.server.WindowsSpeechRecognition;

import java.util.ArrayList;
import java.util.List;

/** {@link RecognizerBackend} over Windows' SAPI recognizers (WindowsSpeechRecognition). */
final class WindowsRecognizerBackend implements RecognizerBackend {
    @Override
    public List<Language> languages() {
        List<Language> out = new ArrayList<>();
        List<String> seen = new ArrayList<>();
        // The default recognizer's languages first, so that its region wins a bare language.
        for (int pass = 0; pass < 2; pass++) {
            for (WindowsSpeechRecognition.Recognizer r : WindowsSpeechRecognition.recognizers()) {
                if (r.isDefault != (pass == 0)) continue;
                for (String tag : r.languages) {
                    if (seen.contains(tag.toLowerCase(java.util.Locale.ROOT))) continue;
                    seen.add(tag.toLowerCase(java.util.Locale.ROOT));
                    out.add(new Language(tag, r.isDefault));
                }
            }
        }
        return out;
    }

    @Override
    public Recognition start(String tag, int sampleRate, boolean partials, int maxResults)
            throws StartFailure {
        WindowsSpeechRecognition.Session session;
        try {
            session = WindowsSpeechRecognition.start(tag, sampleRate, partials, maxResults);
        } catch (WindowsSpeechRecognition.StartException e) {
            int reason = e.reason == WindowsSpeechRecognition.REASON_NO_RECOGNIZER ? NO_RECOGNIZER
                    : e.reason == WindowsSpeechRecognition.REASON_RECOGNIZER_FAILED
                    ? RECOGNIZER_FAILED : WINDOWS_FAILED;
            throw new StartFailure(reason, e.hr, e.getMessage());
        }
        return new Recognition() {
            @Override public boolean write(short[] pcm, int count) { return session.write(pcm, count); }
            @Override public void endAudio() { session.endAudio(); }
            @Override public void cancel() { session.cancel(); }
            @Override public Event next(int timeoutMs) {
                WindowsSpeechRecognition.Event e = session.next(timeoutMs);
                int kind = e.kind >= NONE && e.kind <= FAILED ? e.kind : FAILED;
                return new Event(kind, e.texts, e.confidences, e.hr);
            }
            @Override public void release() { session.release(); }
        };
    }
}
