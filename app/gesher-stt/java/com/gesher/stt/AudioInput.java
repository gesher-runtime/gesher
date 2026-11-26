/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import java.io.IOException;

/**
 * The audio a recognition hears: 16-bit mono PCM. The microphone (AudioRecord, for the
 * calling app: {@link MicrophoneInput}) or the app's own audio
 * (RecognizerIntent.EXTRA_AUDIO_SOURCE: {@link StreamInput}).
 */
interface AudioInput {
    int sampleRate();

    /** Starts capturing; throws when the audio cannot be had. */
    void start() throws IOException;

    /**
     * Reads up to {@code pcm.length} samples, blocking until they come. Returns the count,
     * 0 when there is nothing more (the app closed its audio, or {@link #stop}), and
     * throws on a capture failure.
     */
    int read(short[] pcm) throws IOException;

    /** Stops capturing; a blocked read returns. Callable from any thread. */
    void stop();
}
