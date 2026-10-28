package com.gesher.server;

import android.os.Process;
import android.system.Os;

/** Runs in a fresh real VM; expected UID is an assertion, never an authority. */
public final class PackageUidChild {
    public static void main(String[] args) throws Exception {
        int expected = Integer.parseInt(args[0]);
        check(expected);
        Os.setenv("GESHER_UID", "12345", true);
        Os.setenv("GESHER_BINDER_UID", "1000", true);
        check(expected);
        System.out.println("CHILD_UID_PASS " + expected);
        System.exit(0);
    }

    private static void check(int uid) throws Exception {
        if (Os.getuid() != uid || Os.geteuid() != uid || Process.myUid() != uid) {
            throw new AssertionError("self UID disagrees with captured package identity " + uid);
        }
    }
}
