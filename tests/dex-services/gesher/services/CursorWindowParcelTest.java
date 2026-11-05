/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.database.Cursor;
import android.database.CursorWindow;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.ServiceManager;
import android.os.SystemProperties;

import java.io.FileInputStream;
import java.util.Arrays;
import java.util.Properties;

/** Native CursorWindow JNI/Parcel round trips, locally and over a real remote Binder. */
public final class CursorWindowParcelTest {
    private static final String DESCRIPTOR = "gesher.services.ICursorWindowParcelTest";
    private static final int WINDOW = IBinder.FIRST_CALL_TRANSACTION;
    private static final int EXIT = WINDOW + 1;
    private static final int START = 7;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    private static byte[] blob(boolean large) {
        byte[] result = new byte[large ? 128 * 1024 : 13];
        for (int i = 0; i < result.length; i++) result[i] = (byte) (i * 31 + 17);
        return result;
    }
    private static CursorWindow window(boolean large) {
        CursorWindow window = new CursorWindow("synthetic-cursor-fixture", 2 * 1024 * 1024L);
        window.setStartPosition(START);
        require(window.setNumColumns(5) && window.allocRow(), "Cannot allocate fixture row");
        require(window.putLong(-0x123456789abL, START, 0), "Cannot put integer");
        require(window.putDouble(3.125, START, 1), "Cannot put float");
        require(window.putString("fixture-שלום-☃", START, 2), "Cannot put Unicode text");
        require(window.putBlob(blob(large), START, 3), "Cannot put blob");
        require(window.putNull(START, 4), "Cannot put null");
        return window;
    }
    private static void check(CursorWindow window, boolean large) {
        require(window.getStartPosition() == START && window.getNumRows() == 1,
                "Cursor row positioning was not preserved");
        require(window.getType(START, 0) == Cursor.FIELD_TYPE_INTEGER
                && window.getLong(START, 0) == -0x123456789abL, "Integer was not preserved");
        require(window.getType(START, 1) == Cursor.FIELD_TYPE_FLOAT
                && window.getDouble(START, 1) == 3.125, "Float was not preserved");
        require(window.getType(START, 2) == Cursor.FIELD_TYPE_STRING
                && "fixture-שלום-☃".equals(window.getString(START, 2)), "Unicode text was not preserved");
        require(window.getType(START, 3) == Cursor.FIELD_TYPE_BLOB
                && Arrays.equals(blob(large), window.getBlob(START, 3)), "Blob was not preserved");
        require(window.getType(START, 4) == Cursor.FIELD_TYPE_NULL, "Null field was not preserved");
    }
    private static void local() {
        for (boolean large : new boolean[]{false, true}) {
            Parcel parcel = Parcel.obtain();
            CursorWindow received;
            try {
                try (CursorWindow source = window(large)) { source.writeToParcel(parcel, 0); }
                parcel.setDataPosition(0);
                received = CursorWindow.CREATOR.createFromParcel(parcel);
            } finally { parcel.recycle(); }
            try (CursorWindow closed = received) { check(closed, large); }
        }
        System.out.println("CursorWindowParcelTest: local inline and shared-memory windows preserve types/data after sender and Parcel close");
        System.out.println("CursorWindowParcelTest all passed");
    }
    private static void service(String name) {
        ServiceManager.addService(name, new Binder(DESCRIPTOR) {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) {
                request.enforceInterface(DESCRIPTOR);
                if (code == WINDOW) {
                    boolean large = request.readBoolean();
                    reply.writeNoException();
                    reply.writeInt(Process.myPid());
                    reply.writeInt(Binder.getCallingPid());
                    try (CursorWindow source = window(large)) { source.writeToParcel(reply, 0); }
                    return true;
                }
                if (code == EXIT) {
                    reply.writeNoException();
                    new Thread(() -> {
                        try { Thread.sleep(200); } catch (InterruptedException ignored) { }
                        System.exit(0);
                    }, "cursor-fixture-exit").start();
                    return true;
                }
                return false;
            }
        });
        System.out.println("CursorWindowParcelTest: synthetic Binder service ready");
        System.out.flush();
        Binder.joinThreadPool();
    }
    private static void client(String name) throws Exception {
        IBinder remote = null;
        for (int i = 0; i < 200 && remote == null; i++) {
            remote = ServiceManager.checkService(name);
            if (remote == null) Thread.sleep(50);
        }
        require(remote != null && !(remote instanceof Binder), "Fixture service is not a remote Binder proxy");
        for (boolean large : new boolean[]{false, true}) {
            Parcel request = Parcel.obtain(), response = Parcel.obtain();
            CursorWindow received;
            try {
                request.writeInterfaceToken(DESCRIPTOR); request.writeBoolean(large);
                require(remote.transact(WINDOW, request, response, 0), "Remote window transaction failed");
                response.readException();
                require(response.readInt() != Process.myPid(), "Cursor service ran in the client process");
                require(response.readInt() == Process.myPid(), "Server did not authenticate the real caller PID");
                received = CursorWindow.CREATOR.createFromParcel(response);
            } finally { request.recycle(); response.recycle(); }
            try (CursorWindow closed = received) { check(closed, large); }
        }
        Parcel request = Parcel.obtain(), response = Parcel.obtain();
        try {
            request.writeInterfaceToken(DESCRIPTOR);
            require(remote.transact(EXIT, request, response, 0), "Remote fixture shutdown failed");
            response.readException();
        } finally { request.recycle(); response.recycle(); }
        System.out.println("CursorWindowParcelTest: two-process Binder inline/shared-memory cursor transfer preserves types/data and descriptor lifetime");
        System.out.println("CursorWindowParcelTest all passed");
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Properties properties = new Properties();
        try (FileInputStream input = new FileInputStream(System.getenv("ANDROID_ROOT") + "/build.prop")) {
            properties.load(input);
        }
        for (String key : properties.stringPropertyNames()) {
            try { SystemProperties.set(key.endsWith("?") ? key.substring(0, key.length() - 1) : key,
                    properties.getProperty(key)); }
            catch (RuntimeException alreadyInitialized) { /* Read-only properties retain runtime init values. */ }
        }
        require(android.os.Build.SUPPORTED_ABIS.length > 0, "Runtime ABI properties are unavailable");
        if (args.length == 0) local();
        else if (args[0].equals("service")) service(args[1]);
        else if (args[0].equals("client")) client(args[1]);
        else throw new IllegalArgumentException("Unknown fixture mode");
    }
}
