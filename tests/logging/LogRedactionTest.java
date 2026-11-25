/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Host contracts of the production LogRedaction: the vectors shared with the native sink, the
 * line-buffered stdout/stderr stream (credentials split between writes, multi-line text,
 * non-ASCII), and the one-time scrub of text logs and binary .logcat record streams.
 * Usage: LogRedactionTest &lt;redaction_vectors.inc&gt; &lt;scratch directory&gt;
 */
public final class LogRedactionTest {
    private static int checks, failures;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    static List<String[]> vectors(File file) throws Exception {
        List<String[]> result = new ArrayList<>();
        for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
            if (!line.startsWith("REDACTION_VECTOR(")) continue;
            List<String> strings = new ArrayList<>();
            StringBuilder current = null;
            for (int i = "REDACTION_VECTOR(".length(); i < line.length(); i++) {
                char c = line.charAt(i);
                if (current == null) {
                    if (c == '"') current = new StringBuilder();
                } else if (c == '\\') {
                    char e = line.charAt(++i);
                    current.append(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                } else if (c == '"') {
                    strings.add(current.toString());
                    current = null;
                } else {
                    current.append(c);
                }
            }
            if (strings.size() != 2) throw new AssertionError("bad vector line: " + line);
            result.add(strings.toArray(new String[0]));
        }
        if (result.size() < 10) throw new AssertionError("too few vectors in " + file);
        return result;
    }

    private static String streamed(String text, int chunk, boolean flushEachChunk) throws Exception {
        ByteArrayOutputStream sink = new ByteArrayOutputStream();
        LogRedaction.Stream stream = new LogRedaction.Stream(sink);
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        for (int at = 0; at < bytes.length; at += chunk) {
            stream.write(bytes, at, Math.min(chunk, bytes.length - at));
            if (flushEachChunk) stream.flush();
        }
        stream.flush();
        return sink.toString("UTF-8");
    }

    private static byte[] record(int priority, String tag, String text) {
        byte[] t = tag.getBytes(StandardCharsets.UTF_8), m = text.getBytes(StandardCharsets.UTF_8);
        ByteBuffer b = ByteBuffer.allocate(40 + t.length + m.length).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(0x474c4f47).putInt(40 + t.length + m.length).putLong(133_000_000_000_000_000L)
                .putInt(1234).putInt(5678).putInt(priority).putInt(0).putInt(t.length).putInt(m.length);
        b.put(t).put(m);
        return b.array();
    }

    /** The texts of a record stream, validating every record's framing. */
    private static List<String> texts(byte[] bytes) {
        ByteBuffer in = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        List<String> result = new ArrayList<>();
        for (int at = 0; at < bytes.length;) {
            int size = in.getInt(at + 4), tag = in.getInt(at + 32), text = in.getInt(at + 36);
            if (in.getInt(at) != 0x474c4f47 || size != 40 + tag + text || size > bytes.length - at
                    || in.getInt(at + 16) != 1234 || in.getInt(at + 24) != 3) {
                throw new AssertionError("broken record framing at " + at);
            }
            result.add(new String(bytes, at + 40 + tag, text, StandardCharsets.UTF_8));
            at += size;
        }
        return result;
    }

    public static void main(String[] args) throws Exception {
        List<String[]> vectors = vectors(new File(args[0]));
        File scratch = new File(args[1]);
        StringBuilder all = new StringBuilder(), allExpected = new StringBuilder();
        for (String[] v : vectors) {
            String out = LogRedaction.redact(v[0]);
            check(out.equals(v[1]), "vector <" + v[0] + "> gave <" + out + ">");
            check(LogRedaction.redact(v[1]).equals(v[1]), "idempotent <" + v[1] + ">");
            byte[] bytes = v[1].getBytes(StandardCharsets.UTF_8);
            check(LogRedaction.redact(bytes, 0, bytes.length) == null, "unchanged bytes report null");
            all.append(v[0]).append('\n');
            allExpected.append(v[1]).append('\n');
        }

        // stdout/stderr: lines written whole, byte by byte, in every chunking, flushed or not.
        String text = all.toString(), expected = allExpected.toString();
        for (int chunk : new int[] {1, 2, 3, 5, 7, 64, 4096, 1 << 20}) {
            check(streamed(text, chunk, false).equals(expected), "stream chunk " + chunk);
        }
        // A credential printed in pieces of one line (print("ya2"); print("9...")).
        ByteArrayOutputStream sink = new ByteArrayOutputStream();
        PrintStream out = LogRedaction.printStream(sink, "UTF-8");
        out.print("GmsAuthenticator getAuthToken: ");
        out.print("ya2");
        out.print("9.a0SplitSYNTHETIC");
        out.println("Value");
        out.printf("%s=%s%s%n", "auth_token", "8754", "321SYNTHETIC");
        out.println("multi\nline Authorization: Bearer abcSYNTHETIC\nשלום עולם");
        new IllegalStateException("password=traceSYNTHETIC").printStackTrace(out);
        out.print("unterminated password=");
        out.print("tailSYNTHETIC");
        out.flush();
        // println's line.separator is "\r\n" on a Windows host JVM.
        String printed = sink.toString("UTF-8").replace("\r\n", "\n");
        check(!printed.contains("SYNTHETIC") && !printed.contains("SplitSYN"),
                "print/printf/println/printStackTrace pieces redacted: " + printed);
        check(printed.contains("getAuthToken: <redacted>\n") && printed.contains("auth_token=<redacted>\n")
                && printed.contains("multi\nline Authorization: <redacted>\nשלום עולם\n")
                && printed.contains("password=<redacted>")
                && printed.endsWith("unterminated password=<redacted>"), "redacted output keeps the text: " + printed);
        // A partial line is written at flush (as libcore's autoflush stream), redacted.
        sink.reset();
        LogRedaction.Stream partial = new LogRedaction.Stream(sink);
        partial.write("prompt> password=flushSYNTHETIC".getBytes(StandardCharsets.UTF_8));
        check(sink.size() == 0, "partial line waits for its newline");
        partial.flush();
        check(sink.toString("UTF-8").equals("prompt> password=<redacted>"), "flush writes a redacted partial line");
        // An unterminated run beyond the buffer is bounded and passed on.
        sink.reset();
        LogRedaction.Stream bounded = new LogRedaction.Stream(sink);
        byte[] big = new byte[LogRedaction.Stream.LIMIT * 3 + 17];
        java.util.Arrays.fill(big, (byte) 'x');
        bounded.write(big);
        check(sink.size() == LogRedaction.Stream.LIMIT * 3, "long unterminated output is emitted at the bound");
        bounded.flush();
        check(sink.size() == big.length, "the rest is emitted at flush");

        // One-time scrub of logs written by earlier versions.
        File logs = new File(scratch, "logs");
        logs.mkdirs();
        File text1 = new File(logs, "com.google.android.gms.log");
        Files.write(text1.toPath(), text.getBytes(StandardCharsets.UTF_8));
        File clean = new File(logs, "binderd.log");
        Files.write(clean.toPath(), "nothing secret\n".getBytes(StandardCharsets.UTF_8));
        long cleanModified = clean.lastModified();
        ByteArrayOutputStream stream = new ByteArrayOutputStream();
        for (String[] v : vectors) stream.write(record(3, "GmsAuth", v[0]));
        String grown = "m".repeat(4085) + " Passwd=xy";
        stream.write(record(3, "grown", grown));
        stream.write(record(3, "after", "still framed"));
        File logcat = new File(logs, "com.google.android.gms.log.logcat");
        Files.write(logcat.toPath(), stream.toByteArray());
        File broken = new File(logs, "broken.log.logcat");
        Files.write(broken.toPath(), new byte[] {1, 2, 3, 4, 5});
        int changed = LogRedaction.scrubLogs(logs);
        check(changed == 3, "scrub changed the text log, the record stream and removed the broken one: " + changed);
        check(new String(Files.readAllBytes(text1.toPath()), StandardCharsets.UTF_8).equals(expected),
                "text log rewritten redacted");
        check(clean.lastModified() == cleanModified, "a log without credentials is not rewritten");
        List<String> records = texts(Files.readAllBytes(logcat.toPath()));
        check(records.size() == vectors.size() + 2, "every record kept");
        boolean same = true;
        for (int i = 0; i < vectors.size(); i++) same &= records.get(i).equals(vectors.get(i)[1]);
        check(same, "record texts redacted");
        check(records.get(vectors.size()).equals(("m".repeat(4085) + " Passwd=<redacted>").substring(0, 4096)),
                "grown record bounded to the field");
        check(records.get(vectors.size() + 1).equals("still framed"), "record after a grown one intact");
        check(!broken.exists(), "an unreadable record stream is removed");
        check(new File(logs, LogRedaction.SCRUBBED_MARKER).exists(), "scrub recorded");
        File[] leftovers = logs.listFiles((dir, name) -> name.endsWith(".redacting"));
        check(leftovers != null && leftovers.length == 0, "no temporary files remain");
        // Recorded: later starts do not scan again.
        Files.write(text1.toPath(), "password=laterSYNTHETIC\n".getBytes(StandardCharsets.UTF_8));
        check(LogRedaction.scrubLogs(logs) == 0, "scrub runs once");

        // A file that cannot be replaced is kept and the scrub retried.
        File retry = new File(scratch, "retry");
        retry.mkdirs();
        File held = new File(retry, "held.log");
        Files.write(held.toPath(), "auth_token=heldSYNTHETIC\n".getBytes(StandardCharsets.UTF_8));
        boolean windows = System.getProperty("os.name").startsWith("Windows");
        if (windows) {
            try (FileOutputStream holder = new FileOutputStream(held, true)) {
                LogRedaction.scrubLogs(retry);
                check(!new File(retry, LogRedaction.SCRUBBED_MARKER).exists(), "failed scrub retried later");
            }
        }
        LogRedaction.scrubLogs(retry);
        check(new String(Files.readAllBytes(held.toPath()), StandardCharsets.UTF_8)
                .equals("auth_token=<redacted>\n"), "retried scrub redacts");

        System.out.println("log redaction: " + checks + " checks, " + failures + " failed");
        if (failures > 0) System.exit(1);
    }
}
