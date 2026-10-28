package com.gesher.server;

import android.os.Process;
import android.system.Os;

/** Runs in a fresh real VM; expected UID is an assertion, never an authority. */
public final class PackageUidChild {
    public static void main(String[] args) throws Exception {
        int expected = Integer.parseInt(args[0]);
        check(expected);
        if (args.length > 1) remote(expected, args[1]);
        Os.setenv("GESHER_UID", "12345", true);
        Os.setenv("GESHER_BINDER_UID", "1000", true);
        check(expected);
        if (args.length > 1) remote(expected, args[1]);
        System.out.println("CHILD_UID_PASS " + expected);
        System.exit(0);
    }

    private static void remote(int uid, String name) throws Exception {
        android.os.IBinder receiver = android.os.ServiceManager.getService(name);
        if (receiver == null) throw new AssertionError("remote UID receiver missing");
        android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
        try {
            data.writeInt(uid);
            if (!receiver.transact(android.os.IBinder.FIRST_CALL_TRANSACTION, data, reply, 0)
                    || reply.readInt() != uid) throw new AssertionError("remote Binder UID mismatch");
        } finally { data.recycle(); reply.recycle(); }
    }

    private static void check(int uid) throws Exception {
        if (Os.getuid() != uid || Os.geteuid() != uid || Process.myUid() != uid) {
            throw new AssertionError("self UID disagrees with captured package identity " + uid);
        }
    }
}
