/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import android.media.AudioFormat;

/**
 * The PCM of a RIFF WAV container, as Windows' SpeechSynthesisStream holds it: its
 * format, as SynthesisCallback.start takes it, and where the samples are. The samples
 * are handed over unchanged; a format Android's SynthesisCallback cannot take is
 * refused, not converted.
 */
final class WavPcm {
    private static final int WAVE_FORMAT_PCM = 1;
    private static final int WAVE_FORMAT_IEEE_FLOAT = 3;
    private static final int WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

    /** An AudioFormat.ENCODING_PCM_* value. */
    final int encoding;
    final int channels;
    final int sampleRate;
    final int frameSize;
    final int dataOffset;
    final int dataLength;

    private WavPcm(int encoding, int channels, int sampleRate, int frameSize, int dataOffset,
            int dataLength) {
        this.encoding = encoding;
        this.channels = channels;
        this.sampleRate = sampleRate;
        this.frameSize = frameSize;
        this.dataOffset = dataOffset;
        this.dataLength = dataLength;
    }

    int frames() {
        return dataLength / frameSize;
    }

    /** @throws IllegalArgumentException for anything but 8/16-bit PCM or float, 1-2 channels */
    static WavPcm parse(byte[] b) {
        if (b == null || b.length < 12 || !tag(b, 0, "RIFF") || !tag(b, 8, "WAVE")) {
            throw new IllegalArgumentException("not a RIFF WAVE stream");
        }
        int format = -1, channels = 0, sampleRate = 0, bits = 0;
        int at = 12;
        while (at + 8 <= b.length) {
            long size = le32(b, at + 4);
            int body = at + 8;
            int available = b.length - body;
            if (tag(b, at, "fmt ")) {
                if (size < 16 || available < 16) throw new IllegalArgumentException("short fmt chunk");
                format = le16(b, body);
                channels = le16(b, body + 2);
                sampleRate = (int) Math.min(le32(b, body + 4), Integer.MAX_VALUE);
                bits = le16(b, body + 14);
                if (format == WAVE_FORMAT_EXTENSIBLE) {
                    // WAVEFORMATEXTENSIBLE: the SubFormat GUID starts with the format tag.
                    if (size < 40 || available < 26) {
                        throw new IllegalArgumentException("short extensible fmt chunk");
                    }
                    format = le16(b, body + 24);
                }
            } else if (tag(b, at, "data")) {
                if (format < 0) throw new IllegalArgumentException("data before fmt");
                int encoding;
                if (format == WAVE_FORMAT_PCM && bits == 16) {
                    encoding = AudioFormat.ENCODING_PCM_16BIT;
                } else if (format == WAVE_FORMAT_PCM && bits == 8) {
                    encoding = AudioFormat.ENCODING_PCM_8BIT;
                } else if (format == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
                    encoding = AudioFormat.ENCODING_PCM_FLOAT;
                } else {
                    throw new IllegalArgumentException("format " + format + "/" + bits
                            + " bits is not one SynthesisCallback takes");
                }
                if (channels < 1 || channels > 2) {
                    throw new IllegalArgumentException(channels + " channels");
                }
                if (sampleRate <= 0) throw new IllegalArgumentException("sample rate " + sampleRate);
                int frameSize = channels * bits / 8;
                // A streaming writer may leave the size unset (0xFFFFFFFF): what is there.
                long length = Math.min(size, available);
                length -= length % frameSize;
                return new WavPcm(encoding, channels, sampleRate, frameSize, body, (int) length);
            }
            if (size > available) break;
            at = (int) (body + size + (size & 1));
        }
        throw new IllegalArgumentException("no data chunk");
    }

    private static boolean tag(byte[] b, int at, String tag) {
        for (int i = 0; i < 4; i++) {
            if (b[at + i] != tag.charAt(i)) return false;
        }
        return true;
    }

    private static int le16(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8;
    }

    private static long le32(byte[] b, int at) {
        return ((long) le16(b, at + 2) << 16) | le16(b, at);
    }
}
