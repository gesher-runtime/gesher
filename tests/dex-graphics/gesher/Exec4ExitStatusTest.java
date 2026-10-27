package gesher;

import com.gesher.server.ProcessExitStatus;

/** Generation and PID controls for the actual server exit-result component, run under ART. */
public final class Exec4ExitStatusTest {
    private static int failed;

    private static void check(boolean okay, String name) {
        System.out.println((okay ? "PASS" : "FAIL") + " exec4 exit: " + name);
        if (!okay) failed++;
    }

    public static void main(String[] args) throws Exception {
        ProcessExitStatus status = new ProcessExitStatus();
        String pkg = "com.gesher.exitstatus";
        check(status.code(pkg) == null, "an unobserved process has no success code");
        status.started(pkg, 1);
        check(!status.exited(pkg, 1, 100, 0) && status.code(pkg) == null,
                "a result cannot precede binding its actual PID");
        check(status.bind(pkg, 1, 100) && status.exited(pkg, 1, 100, 0) && status.code(pkg) == 0,
                "the first process publishes its observed zero exit");
        status.started(pkg, 2);
        check(status.code(pkg) == null, "a new start clears the previous package success");
        check(!status.exited(pkg, 1, 100, 23) && status.code(pkg) == null,
                "a late previous-process exit cannot fill the new start's result");
        check(status.bind(pkg, 2, 101) && !status.bind(pkg, 2, 100)
                        && !status.exited(pkg, 2, 100, 0) && status.code(pkg) == null,
                "a matching generation still requires the matching PID");
        check(status.exited(pkg, 2, 101, 23) && status.code(pkg) == 23,
                "the latest abnormal exit replaces the old success");
        check(!status.exited(pkg, 1, 100, 0) && !status.exited(pkg, 2, 101, 0)
                        && status.code(pkg) == 23,
                "late and conflicting duplicate publications cannot turn failure into success");
        status.started(pkg, 3);
        check(status.bind(pkg, 3, 101) && !status.exited(pkg, 2, 101, 0)
                        && status.code(pkg) == null,
                "PID reuse cannot reuse the previous start sequence");
        check(status.exited(pkg, 3, 101, 0) && status.code(pkg) == 0,
                "a later normal process publishes its own fresh success");
        Thread old = new Thread(() -> {
            for (int i = 0; i < 1000; i++) status.exited(pkg, 2, 101, 23);
        });
        old.start();
        status.started("com.gesher.other", 4);
        status.bind("com.gesher.other", 4, 102);
        status.exited("com.gesher.other", 4, 102, 17);
        old.join();
        check(status.code(pkg) == 0 && status.code("com.gesher.other") == 17,
                "concurrent stale callbacks and other packages remain isolated");
        if (failed != 0) throw new AssertionError(failed + " exit-status test(s) failed");
        System.out.println("Exec4ExitStatusTest all passed");
    }
}
