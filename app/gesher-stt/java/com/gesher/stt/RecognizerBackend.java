/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import java.util.List;

/**
 * What the recognition service needs from Windows, in Windows' own terms. The only
 * production implementation is {@link WindowsRecognizerBackend}; test doubles live in
 * tests/speech and are compiled only by the host contracts.
 */
interface RecognizerBackend {
    /** Event kinds, as WindowsSpeechRecognition's. */
    int NONE = 0;
    int SOUND_START = 1;
    int SOUND_END = 2;
    int HYPOTHESIS = 3;
    int RECOGNITION = 4;
    int NO_MATCH = 5;
    int STREAM_END = 6;
    int FAILED = 7;

    /** Why a recognition did not start. */
    int NO_RECOGNIZER = 2;
    int RECOGNIZER_FAILED = 3;
    int WINDOWS_FAILED = 1;

    /** One installed recognizer's language: a BCP-47 tag Windows reported. */
    final class Language {
        final String tag;
        /** The language of Windows' default recognizer. */
        final boolean ofDefaultRecognizer;

        Language(String tag, boolean ofDefaultRecognizer) {
            this.tag = tag;
            this.ofDefaultRecognizer = ofDefaultRecognizer;
        }
    }

    final class Event {
        final int kind;
        final String[] texts;
        final float[] confidences;
        final int hr;

        Event(int kind, String[] texts, float[] confidences, int hr) {
            this.kind = kind;
            this.texts = texts == null ? new String[0] : texts;
            this.confidences = confidences == null ? new float[0] : confidences;
            this.hr = hr;
        }
    }

    final class StartFailure extends Exception {
        final int reason;
        final int hr;

        StartFailure(int reason, int hr, String message) {
            super(message);
            this.reason = reason;
            this.hr = hr;
        }
    }

    /** One dictation over written PCM; {@link #cancel} may come from any thread. */
    interface Recognition {
        boolean write(short[] pcm, int count);
        void endAudio();
        void cancel();
        Event next(int timeoutMs);
        void release();
    }

    /** The installed recognizers' languages; empty when Windows has none. */
    List<Language> languages();

    Recognition start(String tag, int sampleRate, boolean partials, int maxResults)
            throws StartFailure;
}
