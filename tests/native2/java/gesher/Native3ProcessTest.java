package gesher;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/** The framework's proc natives use real procfs and the pinned field format. */
public final class Native3ProcessTest {
    private static Method read;
    private static Method readFile;
    private static Method parse;
    private static final int STRING = 0x1000;
    private static final int LONG = 0x2000;
    private static final int FLOAT = 0x4000;

    private static void check(String name, boolean good) {
        if (!good) throw new AssertionError(name);
        System.out.println("PASS  native3 process: " + name);
    }

    private static void rejects(Class<? extends Throwable> type, String file,
            String[] fields, long[] values) throws Exception {
        try {
            read.invoke(null, file, fields, values);
            throw new AssertionError("expected " + type.getName());
        } catch (InvocationTargetException e) {
            check(type.getSimpleName() + " for invalid proc-line arguments",
                    type.isInstance(e.getCause()));
        }
    }

    private static boolean line(byte[] bytes, int start, int end, int[] format,
            String[] strings, long[] longs, float[] floats) throws Exception {
        return (Boolean) parse.invoke(null, bytes, start, end, format, strings, longs, floats);
    }

    private static void fields(File fixture) throws Exception {
        byte[] mixed = "ignored 4294967297 -17 1.25 tail".getBytes(StandardCharsets.US_ASCII);
        String[] strings = {"keep", "keep", "keep", "keep", "tail"};
        long[] longs = {99, 99, 99, 99, 99};
        float[] floats = {99, 99, 99, 99, 99};
        check("shared output index across skipped, integer, float and string fields",
                line(mixed, 0, mixed.length, new int[] {' ', ' ' | LONG, ' ' | LONG,
                        ' ' | FLOAT, STRING}, strings, longs, floats)
                        && longs[0] == 4294967297L && longs[1] == -17
                        && longs[2] == 99 && floats[2] == 1.25f && floats[0] == 99
                        && strings[3].equals("tail") && strings[0].equals("keep")
                        && longs[4] == 99);
        byte[] grouped = "xx (two words)   \"quoted words\" Z\n".getBytes(StandardCharsets.US_ASCII);
        String[] groups = new String[3];
        long[] chars = {-1, -1, -1};
        check("bounded slices, parens, quotes, combined delimiters and character output",
                line(grouped, 3, grouped.length, new int[] {' ' | 0x200 | 0x100 | STRING,
                        ' ' | 0x400 | STRING, '\n' | 0x800 | LONG}, groups, chars, null)
                        && groups[0].equals("two words") && groups[1].equals("quoted words")
                        && chars[0] == -1 && chars[2] == 'Z');
        byte[] plain = "plain value".getBytes(StandardCharsets.US_ASCII);
        check("unquoted field and short output arrays follow the pinned format",
                line(plain, 0, plain.length, new int[] {' ' | 0x400 | STRING, STRING},
                        groups = new String[1], null, null) && groups[0].equals("plain"));
        byte[] dirty = {'a', '\t', 'b', 0};
        check("nonprinting string bytes are sanitized in the output and input",
                line(dirty, 0, dirty.length, new int[] {STRING}, groups, null, null)
                        && groups[0].equals("a?b") && dirty[1] == '?');
        byte[] partial = {'7', ' '};
        long[] values = {-1, 42};
        check("truncated input returns false while preserving parsed and untouched slots",
                !line(partial, 0, partial.length, new int[] {' ' | LONG, LONG},
                        null, values, null) && values[0] == 7 && values[1] == 42);
        check("one field may populate all three output types",
                line(new byte[] {'2', '5'}, 0, 2, new int[] {STRING | LONG | FLOAT},
                        groups, values, floats)
                        && groups[0].equals("25") && values[0] == 25 && floats[0] == 25f);
        check("empty format accepts empty input without output arrays",
                line(new byte[0], 0, 0, new int[0], null, null, null));
        try (FileOutputStream out = new FileOutputStream(fixture)) {
            out.write(("x".repeat(8192) + " 4294967297 -17 1.25")
                    .getBytes(StandardCharsets.US_ASCII));
        }
        check("readProcFile grows past a small proc buffer and preserves numeric widths",
                (Boolean) readFile.invoke(null, fixture.getPath(),
                        new int[] {' ', ' ' | LONG, ' ' | LONG, FLOAT}, null, values, floats)
                        && values[0] == 4294967297L && values[1] == -17 && floats[2] == 1.25f);
        values[0] = 123;
        check("readProcFile missing path returns false without changing output",
                !(Boolean) readFile.invoke(null, fixture.getPath() + ".missing",
                        new int[] {LONG}, null, values, null) && values[0] == 123);
        long[] pages = {-1, -1};
        check("readProcFile reads actual procfs memory counters",
                (Boolean) readFile.invoke(null, "/proc/self/statm",
                        new int[] {' ' | LONG, ' ' | LONG}, null, pages, null)
                        && pages[0] > 0 && pages[1] > 0);
        for (Object[] invalid : new Object[][] {
                {readFile, new Object[] {null, new int[0], null, null, null}, NullPointerException.class},
                {readFile, new Object[] {fixture.getPath(), null, null, null, null}, NullPointerException.class},
                {parse, new Object[] {null, 0, 0, new int[0], null, null, null}, NullPointerException.class},
                {parse, new Object[] {partial, 0, 2, null, null, null, null}, NullPointerException.class},
                {parse, new Object[] {partial, -1, 2, new int[0], null, null, null}, ArrayIndexOutOfBoundsException.class},
                {parse, new Object[] {partial, 0, 3, new int[0], null, null, null}, ArrayIndexOutOfBoundsException.class}}) {
            try {
                ((Method) invalid[0]).invoke(null, (Object[]) invalid[1]);
                throw new AssertionError("invalid proc-file/parser arguments accepted");
            } catch (InvocationTargetException e) {
                check("proc-file/parser invalid argument " + invalid[2],
                        ((Class<?>) invalid[2]).isInstance(e.getCause()));
            }
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        read = Class.forName("android.os.Process").getDeclaredMethod("readProcLines",
                String.class, String[].class, long[].class);
        readFile = Class.forName("android.os.Process").getDeclaredMethod("readProcFile",
                String.class, int[].class, String[].class, long[].class, float[].class);
        parse = Class.forName("android.os.Process").getDeclaredMethod("parseProcLine",
                byte[].class, int.class, int.class, int[].class,
                String[].class, long[].class, float[].class);
        long[] live = {-1, -1, -1, 77};
        read.invoke(null, "/proc/self/status",
                new String[] {"Threads:", "VmRSS:", "MissingField:"}, live);
        check("real thread/RSS counters, missing field zero, untouched output tail",
                live[0] > 0 && live[1] > 0 && live[2] == 0 && live[3] == 77);
        File fixture = new File(args[0]);
        try {
            try (FileOutputStream out = new FileOutputStream(fixture)) {
                out.write(("Unrelated: 7\nLarge:\t4294967297 kB\nBlank: text\n")
                        .getBytes(StandardCharsets.US_ASCII));
            }
            long[] parsed = {-1, -1, -1, 99};
            read.invoke(null, fixture.getPath(),
                    new String[] {"Large:", "Blank:", "Absent:"}, parsed);
            check("64-bit decimal parsing, units/whitespace and nonnumeric/missing zero",
                    parsed[0] == 4294967297L && parsed[1] == 0
                            && parsed[2] == 0 && parsed[3] == 99);
            long[] absent = {123};
            read.invoke(null, fixture.getPath() + ".missing", new String[] {"Large:"}, absent);
            check("missing file preserves the caller's output", absent[0] == 123);
            rejects(NullPointerException.class, null, new String[0], new long[0]);
            rejects(NullPointerException.class, fixture.getPath(), null, new long[0]);
            rejects(NullPointerException.class, fixture.getPath(), new String[0], null);
            rejects(NullPointerException.class, fixture.getPath(), new String[] {null}, new long[1]);
            rejects(IllegalArgumentException.class, fixture.getPath(),
                    new String[] {"Large:"}, new long[0]);
            fields(fixture);
        } finally {
            if (fixture.exists() && !fixture.delete()) throw new AssertionError("fixture cleanup");
        }
        System.out.println("native3 process: all passed");
    }
}
