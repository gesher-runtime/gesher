/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.io.UnsupportedEncodingException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * Credentials kept out of the log files Gesher persists (docs/logging.md). Android's logcat
 * is an in-memory buffer only privileged readers see; Gesher writes each process's log and
 * stdout/stderr to files in the profile, so bearer tokens and passwords are replaced with
 * {@code <redacted>} before they reach those files. The same rules as the native sink's
 * {@code gesher::logging::RedactCredentials} (compat/posix/src/logging.cc), over bytes: only
 * ASCII is matched and other bytes (UTF-8 text) pass unchanged.
 */
public final class LogRedaction {
    private LogRedaction() {}

    static final byte[] REDACTED = "<redacted>".getBytes(StandardCharsets.US_ASCII);
    private static final String[] PREFIXES = {"ya29.", "aas_et/", "oauth2_4/", "1//"};
    private static final int[] PREFIX_MINIMUM = {1, 1, 1, 20};
    private static final boolean[] PREFIX_BOUNDARY = {false, false, false, true};
    private static final String[] KEY_SUFFIXES = {"token", "secret", "password", "passwd", "cookie"};
    private static final String[] KEY_NAMES = {"authorization", "auth", "apikey", "api_key"};

    /** The text with credential values replaced, or {@code text} itself when it holds none. */
    public static String redact(String text) {
        if (text == null) return null;
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        byte[] redacted = redact(bytes, 0, bytes.length);
        return redacted == null ? text : new String(redacted, StandardCharsets.UTF_8);
    }

    /** {@code len} bytes at {@code off} redacted, or null when nothing was replaced. */
    public static byte[] redact(byte[] b, int off, int len) {
        ByteArrayOutputStream out = null;
        int copied = off, end = off + len;
        int[] span = new int[2];
        for (int at = off; at < end;) {
            if (!secret(b, off, end, at, span)) {
                ++at;
                continue;
            }
            if (out == null) out = new ByteArrayOutputStream(len + REDACTED.length);
            out.write(b, copied, span[0] - copied);
            out.write(REDACTED, 0, REDACTED.length);
            copied = at = span[1];
        }
        if (out == null) return null;
        out.write(b, copied, end - copied);
        return out.toByteArray();
    }

    private static boolean alphanumeric(int c) {
        return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
    }

    private static boolean tokenChar(int c) {
        return alphanumeric(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '+'
                || c == '/' || c == '=';
    }

    private static boolean keyChar(int c) {
        return alphanumeric(c) || c == '_' || c == '-';
    }

    private static boolean blank(int c) {
        return c == ' ' || c == '\t';
    }

    private static boolean valueEnd(int c) {
        return blank(c) || c == '\r' || c == '\n' || c == ',' || c == ';' || c == '&' || c == ')'
                || c == '}' || c == ']' || c == '"' || c == '\'';
    }

    /** Whether {@code size} bytes at {@code at} equal the lower-case ASCII {@code word}. */
    private static boolean sameWord(byte[] b, int at, int size, String word) {
        if (size != word.length()) return false;
        for (int i = 0; i < size; i++) {
            int c = b[at + i] & 0xff;
            if ((c >= 'A' && c <= 'Z' ? c | 0x20 : c) != word.charAt(i)) return false;
        }
        return true;
    }

    private static boolean endsWithWord(byte[] b, int at, int size, String word) {
        return size >= word.length() && sameWord(b, at + size - word.length(), word.length(), word);
    }

    private static boolean credentialKey(byte[] b, int at, int size) {
        if (size == 0) return false;
        for (String suffix : KEY_SUFFIXES) if (endsWithWord(b, at, size, suffix)) return true;
        for (String name : KEY_NAMES) if (sameWord(b, at, size, name)) return true;
        return false;
    }

    private static boolean lineValue(byte[] b, int at, int size) {
        return sameWord(b, at, size, "authorization") || endsWithWord(b, at, size, "cookie");
    }

    /** The secret bytes [span[0], span[1]) of a credential starting at {@code at}, if any. */
    private static boolean secret(byte[] s, int start, int n, int at, int[] span) {
        int c = s[at] & 0xff;
        boolean boundary = at == start || !alphanumeric(s[at - 1] & 0xff);
        for (int p = 0; p < PREFIXES.length; p++) {
            String prefix = PREFIXES[p];
            int length = prefix.length();
            if (c != prefix.charAt(0) || (PREFIX_BOUNDARY[p] && !boundary) || n - at < length) continue;
            boolean match = true;
            for (int i = 1; i < length && match; i++) match = (s[at + i] & 0xff) == prefix.charAt(i);
            if (!match) continue;
            int j = at + length;
            while (j < n && tokenChar(s[j] & 0xff)) ++j;
            if (j - (at + length) >= PREFIX_MINIMUM[p]) {
                span[0] = at + length;
                span[1] = j;
                return true;
            }
        }
        if ((c | 0x20) == 'b' && boundary && n - at > 7 && sameWord(s, at, 6, "bearer")
                && blank(s[at + 6])) {
            int v = at + 6;
            while (v < n && blank(s[v])) ++v;
            int j = v;
            while (j < n && tokenChar(s[j] & 0xff)) ++j;
            if (j > v) {
                span[0] = v;
                span[1] = j;
                return true;
            }
        }
        if (c != '=' && c != ':') return false;
        int k = at;
        while (k > start && blank(s[k - 1])) --k;
        if (k > start && (s[k - 1] == '"' || s[k - 1] == '\'')) {
            --k;
            if (k > start && s[k - 1] == '\\') --k;
        }
        int keyEnd = k;
        while (k > start && keyEnd - k < 64 && keyChar(s[k - 1] & 0xff)) --k;
        if (!credentialKey(s, k, keyEnd - k)) return false;
        int v = at + 1;
        while (v < n && blank(s[v])) ++v;
        int j = v;
        boolean nested = v + 1 < n && s[v] == '\\' && (s[v + 1] == '"' || s[v + 1] == '\'');
        if (nested || (v < n && (s[v] == '"' || s[v] == '\''))) {
            // A quoted value ends at its closing quote, not at an escaped one. In an escaped
            // string (\"key\":\"value\") each \x is one character of the quoted text.
            if (nested) v++;
            byte quote = s[v];
            j = ++v;
            boolean escape = false;
            while (j < n) {
                int unit = nested && s[j] == '\\' && j + 1 < n ? 2 : 1;
                byte d = s[j + unit - 1];
                if (d == '\n' || d == '\r' || (!escape && d == quote)) break;
                escape = !escape && d == '\\';
                j += unit;
            }
        } else if (lineValue(s, k, keyEnd - k)) {
            while (j < n && s[j] != '\n' && s[j] != '\r') ++j;
            while (j > v && blank(s[j - 1])) --j;
        } else {
            while (j < n && !valueEnd(s[j])) ++j;
        }
        if (j == v || (j - v == REDACTED.length && regionEquals(s, v, REDACTED))) return false;
        span[0] = v;
        span[1] = j;
        return true;
    }

    private static boolean regionEquals(byte[] b, int at, byte[] word) {
        for (int i = 0; i < word.length; i++) if (b[at + i] != word[i]) return false;
        return true;
    }

    /**
     * An output stream that redacts each line before passing it on. Complete lines are written
     * as they arrive; a partial line waits for its newline, a flush, or {@link #LIMIT} bytes, so
     * a credential split between two writes of one line is still recognized.
     */
    public static final class Stream extends OutputStream {
        static final int LIMIT = 8192;
        private final OutputStream out;
        private final byte[] line = new byte[LIMIT];
        private int size;
        /** Whether anything written so far was redacted. */
        boolean redacted;

        public Stream(OutputStream out) {
            this.out = out;
        }

        @Override public synchronized void write(int b) throws IOException {
            line[size++] = (byte) b;
            if (b == '\n' || size == LIMIT) emit(size);
        }

        @Override public synchronized void write(byte[] b, int off, int len) throws IOException {
            if (off < 0 || len < 0 || len > b.length - off) throw new IndexOutOfBoundsException();
            while (len > 0) {
                int take = Math.min(len, LIMIT - size);
                System.arraycopy(b, off, line, size, take);
                int scanned = size;
                size += take;
                off += take;
                len -= take;
                int newline = -1;
                for (int i = size - 1; i >= scanned; i--) {
                    if (line[i] == '\n') {
                        newline = i;
                        break;
                    }
                }
                if (newline >= 0) emit(newline + 1);
                else if (size == LIMIT) emit(size);
            }
        }

        @Override public synchronized void flush() throws IOException {
            if (size > 0) emit(size);
            out.flush();
        }

        @Override public synchronized void close() throws IOException {
            flush();
            out.close();
        }

        /** Writes the first {@code count} buffered bytes, redacted, keeping the rest. */
        private void emit(int count) throws IOException {
            byte[] redacted = redact(line, 0, count);
            if (redacted == null) {
                out.write(line, 0, count);
            } else {
                this.redacted = true;
                out.write(redacted);
            }
            System.arraycopy(line, count, line, 0, size - count);
            size -= count;
        }
    }

    /**
     * Replaces System.out and System.err with redacting streams over the same descriptors. The
     * process's stdout and stderr are the package's (or the system server's) persisted .log
     * file. Each complete line is written as soon as it is printed, as libcore's autoflushing
     * streams do; a partial line is written at an explicit flush or at exit.
     */
    public static synchronized void installStandardStreams() {
        if (installed) return;
        installed = true;
        System.out.flush();
        System.err.flush();
        PrintStream out = printStream(new FileOutputStream(FileDescriptor.out),
                System.getProperty("sun.stdout.encoding"));
        PrintStream err = printStream(new FileOutputStream(FileDescriptor.err),
                System.getProperty("sun.stderr.encoding"));
        System.setOut(out);
        System.setErr(err);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            out.flush();
            err.flush();
        }, "gesher-log-flush"));
    }

    private static boolean installed;

    /**
     * A redacting PrintStream over {@code raw}. It does not autoflush: PrintStream flushes after
     * every print when it does, which would write a credential printed in pieces
     * ({@code print("ya2"); print("9.")}) as separate, unrecognizable fragments.
     */
    static PrintStream printStream(OutputStream raw, String encoding) {
        OutputStream out = new Stream(raw);
        if (encoding != null) {
            try {
                return new PrintStream(out, false, encoding);
            } catch (UnsupportedEncodingException e) {
                // As libcore: the default encoding.
            }
        }
        return new PrintStream(out, false);
    }

    /** Marks a logs directory whose files were written, or rewritten, with redaction. */
    static final String SCRUBBED_MARKER = ".credentials-redacted-v1";
    private static final int LOGCAT_MAGIC = 0x474c4f47;
    private static final int LOGCAT_HEADER = 40;
    private static final int LOGCAT_LIMIT = 16 * 1024 * 1024;

    /**
     * Logs written before redaction existed are rewritten once, before the device's processes
     * start (Launcher). Text logs are redacted line by line; a binary .logcat file is rewritten
     * record by record with consistent lengths; a broken tail is dropped, and a file that
     * does not start with a valid record (its reader rejects it) is removed. A file that cannot be replaced (in use) is kept and the
     * scrub is retried at the next start. Returns the number of files changed.
     */
    public static int scrubLogs(File dir) {
        File marker = new File(dir, SCRUBBED_MARKER);
        if (marker.exists() || !dir.isDirectory()) return 0;
        File[] files = dir.listFiles();
        if (files == null) return 0;
        int changed = 0;
        boolean complete = true;
        for (File file : files) {
            String name = file.getName();
            if (!file.isFile()) continue;
            try {
                if (name.endsWith(".redacting")) {
                    // Left by a scrub that was interrupted; the log it was made from remains.
                    Files.deleteIfExists(file.toPath());
                } else if (name.endsWith(".logcat")) {
                    if (scrubLogcat(file)) changed++;
                } else if (name.endsWith(".log")) {
                    if (scrubText(file)) changed++;
                }
            } catch (IOException | RuntimeException e) {
                complete = false;
                System.err.println("gesher: cannot redact credentials in " + file + ": " + e);
            }
        }
        if (complete) {
            try {
                marker.createNewFile();
            } catch (IOException e) {
                System.err.println("gesher: cannot record the log redaction: " + e);
            }
        }
        return changed;
    }

    /** Rewrites a text log through {@link Stream}; true if anything was redacted. */
    static boolean scrubText(File file) throws IOException {
        File temporary = new File(file.getPath() + ".redacting");
        Stream redacting;
        try (InputStream in = new FileInputStream(file);
                Stream out = new Stream(new BufferedOutputStream(
                        new FileOutputStream(temporary), 65536))) {
            redacting = out;
            byte[] buffer = new byte[65536];
            int count;
            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
        } catch (IOException | RuntimeException e) {
            Files.deleteIfExists(temporary.toPath());
            throw e;
        }
        return replace(file, temporary, redacting.redacted);
    }

    /** Rewrites a GESHER_LOG_FILE record stream with redacted text; true if it changed. */
    static boolean scrubLogcat(File file) throws IOException {
        long length = file.length();
        if (length == 0) return false;
        if (length > LOGCAT_LIMIT) return deleteInvalid(file);
        byte[] bytes = Files.readAllBytes(file.toPath());
        ByteBuffer in = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        ByteArrayOutputStream out = new ByteArrayOutputStream(bytes.length);
        boolean changed = false;
        for (int at = 0; at < bytes.length;) {
            boolean valid = bytes.length - at >= LOGCAT_HEADER;
            int size = 0, tagSize = 0, textSize = 0;
            if (valid) {
                size = in.getInt(at + 4);
                tagSize = in.getInt(at + 32);
                textSize = in.getInt(at + 36);
                valid = in.getInt(at) == LOGCAT_MAGIC && tagSize >= 0 && tagSize <= 4096
                        && textSize >= 0 && textSize <= 4096
                        && size == LOGCAT_HEADER + tagSize + textSize && size <= bytes.length - at;
            }
            if (!valid) {
                // Not a record stream at all: removed. A broken tail (a writer killed in an
                // append) is dropped and the records before it are kept, readable again.
                if (at == 0) return deleteInvalid(file);
                changed = true;
                break;
            }
            int textAt = at + LOGCAT_HEADER + tagSize;
            byte[] redacted = redact(bytes, textAt, textSize);
            if (redacted == null) {
                out.write(bytes, at, size);
            } else {
                changed = true;
                int newSize = Math.min(redacted.length, 4096);
                ByteBuffer header = ByteBuffer.allocate(LOGCAT_HEADER).order(ByteOrder.LITTLE_ENDIAN);
                header.put(bytes, at, LOGCAT_HEADER);
                header.putInt(4, LOGCAT_HEADER + tagSize + newSize);
                header.putInt(36, newSize);
                out.write(header.array(), 0, LOGCAT_HEADER);
                out.write(bytes, at + LOGCAT_HEADER, tagSize);
                out.write(redacted, 0, newSize);
            }
            at += size;
        }
        if (!changed) return false;
        File temporary = new File(file.getPath() + ".redacting");
        try (FileOutputStream stream = new FileOutputStream(temporary)) {
            out.writeTo(stream);
        }
        return replace(file, temporary, true);
    }

    private static boolean deleteInvalid(File file) throws IOException {
        Files.delete(file.toPath());
        return true;
    }

    private static boolean replace(File file, File temporary, boolean changed) throws IOException {
        if (!changed) {
            Files.deleteIfExists(temporary.toPath());
            return false;
        }
        try {
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException e) {
            Files.deleteIfExists(temporary.toPath());
            throw e;
        }
        return true;
    }
}
