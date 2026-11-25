package gesher;

/**
 * eglDestroyContext on one guest thread while another keeps drawing (issue
 * #485; tests/native2/egl_context_destroy_jni.cpp) in dalvikvm:
 * {@code EglContextDestroyTest <libeglcontextdestroy.so>}. System.load of the
 * library (x86_64 or arm64-v8a), then its scenarios; prints their lines.
 */
public final class EglContextDestroyTest {
    private static native String run();

    public static void main(String[] args) {
        System.load(args[0]);
        String report = run();
        System.out.print(report);
        System.out.flush();
        if (!report.contains("egl_context_destroy all passed")) System.exit(1);
    }
}
