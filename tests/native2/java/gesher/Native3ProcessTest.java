package gesher;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/** The framework's proc-line native uses real procfs and full-width counters. */
public final class Native3ProcessTest {
    private static Method read;

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

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        read = Class.forName("android.os.Process").getDeclaredMethod("readProcLines",
                String.class, String[].class, long[].class);
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
        } finally {
            if (fixture.exists() && !fixture.delete()) throw new AssertionError("fixture cleanup");
        }
        System.out.println("native3 process: all passed");
    }
}
