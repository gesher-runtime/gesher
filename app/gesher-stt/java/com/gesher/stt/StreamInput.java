/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.stt;

import android.media.AudioFormat;
import android.os.ParcelFileDescriptor;

import java.io.IOException;
import java.io.InputStream;

/**
 * The app's own audio (RecognizerIntent.EXTRA_AUDIO_SOURCE): raw PCM read from its pipe
 * or file until it is closed. Only what Windows' recognizer takes as it is handed over is
 * accepted: ENCODING_PCM_16BIT, one channel, 8 to 48 kHz (the extras' defaults are
 * ENCODING_PCM_16BIT, 1 channel and 16000 Hz).
 */
final class StreamInput implements AudioInput {
    private final ParcelFileDescriptor mSource;
    private final int mSampleRate;
    private InputStream mIn;
    private volatile boolean mStopped;
    private int mPending = -1;  // an odd byte waiting for its pair

    StreamInput(ParcelFileDescriptor source, int sampleRate) {
        mSource = source;
        mSampleRate = sampleRate;
    }

    /** Whether the extras describe audio this input can hand over. */
    static boolean accepts(int encoding, int channels, int sampleRate) {
        return encoding == AudioFormat.ENCODING_PCM_16BIT && channels == 1
                && sampleRate >= 8000 && sampleRate <= 48000;
    }

    @Override
    public int sampleRate() {
        return mSampleRate;
    }

    @Override
    public void start() throws IOException {
        mIn = new ParcelFileDescriptor.AutoCloseInputStream(mSource);
    }

    /**
     * Fills {@code pcm} with the samples the pipe has delivered, at least one: a short read that
     * ends mid-sample (one byte of two) keeps reading until the sample is complete. 0 only at
     * the end of the audio or after {@link #stop()}. A single trailing byte at the end of the
     * stream is half a sample and is dropped: no 16-bit value can be made of it.
     */
    @Override
    public int read(short[] pcm) throws IOException {
        if (mStopped || mIn == null || pcm.length == 0) return 0;
        byte[] bytes = new byte[pcm.length * 2];
        int filled = 0;
        if (mPending >= 0) {
            bytes[filled++] = (byte) mPending;
            mPending = -1;
        }
        while (filled < 2) {
            int n;
            try {
                n = mIn.read(bytes, filled, bytes.length - filled);
            } catch (IOException e) {
                if (mStopped) return 0;
                throw e;
            }
            if (n < 0) return 0;  // end of the audio; a lone byte is dropped
            filled += n;
            if (mStopped) return 0;
        }
        if ((filled & 1) != 0) mPending = bytes[--filled] & 0xff;
        for (int i = 0; i < filled / 2; i++) {  // little-endian, as Android's PCM
            pcm[i] = (short) ((bytes[2 * i] & 0xff) | (bytes[2 * i + 1] << 8));
        }
        return filled / 2;
    }

    @Override
    public void stop() {
        mStopped = true;
        try {
            if (mIn != null) mIn.close();
            else mSource.close();
        } catch (IOException ignored) {
            // Already closed.
        }
    }
}
