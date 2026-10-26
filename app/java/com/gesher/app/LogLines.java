/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;

/** Frames a single reader's byte chunks into atomic stdout lines. */
public final class LogLines {
    private final PrintStream output;
    private final ByteArrayOutputStream pending = new ByteArrayOutputStream();
    public LogLines(PrintStream output) { this.output = output; }
    public void write(byte[] bytes, int offset, int count) {
        int end = offset + count;
        int start = offset;
        for (int at = offset; at < end; ++at) {
            if (bytes[at] == '\n') {
                pending.write(bytes, start, at + 1 - start);
                emit();
                start = at + 1;
            }
        }
        pending.write(bytes, start, end - start);
    }
    public void reset() { pending.reset(); }
    public void finish() {
        if (pending.size() != 0) {
            // Separate an unterminated child fragment from the next parent line.
            pending.write('\n');
            emit();
        }
    }
    private void emit() {
        byte[] line = pending.toByteArray();
        synchronized (output) {
            output.write(line, 0, line.length);
            output.flush();
        }
        pending.reset();
    }
}
