/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import java.util.List;

/**
 * Where the engine's voices and audio come from. Production: {@link WindowsSpeechBackend},
 * Windows' own speech synthesis. The engine's host contracts
 * (tools/test_tts_engine_contracts.py) use an isolated test double.
 */
interface SpeechBackend {
    /** {@link Synthesis#await}'s results. */
    int DONE = 0;
    int CANCELED = 1;
    int FAILED = 2;

    /** Why {@link #start} failed. */
    int REASON_WINDOWS = 1;
    int REASON_NO_SUCH_VOICE = 2;
    int REASON_PROSODY_UNSUPPORTED = 3;

    /** Windows' SpeakingRate and AudioPitch ranges. */
    double MIN_RATE = 0.5;
    double MAX_RATE = 6.0;
    double MIN_PITCH = 0.0;
    double MAX_PITCH = 2.0;

    /** An installed voice. */
    final class BackendVoice {
        final String id;
        final String displayName;
        /** A BCP-47 language tag, such as "he-IL". */
        final String language;
        final boolean female;
        final boolean isDefault;

        BackendVoice(String id, String displayName, String language, boolean female,
                boolean isDefault) {
            this.id = id;
            this.displayName = displayName;
            this.language = language;
            this.female = female;
            this.isDefault = isDefault;
        }
    }

    final class StartFailure extends Exception {
        final int reason;

        StartFailure(int reason, String message) {
            super(message);
            this.reason = reason;
        }
    }

    /** One utterance being rendered: await on the synthesis thread, cancel from any. */
    interface Synthesis {
        int await(long timeoutMs);

        /** The rendered stream, a RIFF WAV container; after {@link #DONE}. */
        byte[] audio();

        /** [startTicks (100 ns), first, last (inclusive)] per word; after {@link #DONE}. */
        long[] words();

        void cancel();

        void release();
    }

    /** The installed voices; empty when there are none. */
    List<BackendVoice> voices();

    Synthesis start(String text, String voiceId, double rate, double pitch, boolean words)
            throws StartFailure;
}
