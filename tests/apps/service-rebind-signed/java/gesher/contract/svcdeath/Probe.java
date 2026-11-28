// SPDX-License-Identifier: Apache-2.0
package gesher.contract.svcdeath;

import android.app.Activity;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.util.Log;

public final class Probe extends Activity implements ServiceConnection {
    private static final String TAG = "ServiceDeathProbe";
    private static final int GET_PID = IBinder.FIRST_CALL_TRANSACTION;
    private static final int EXIT = GET_PID + 1;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private int initialPid, disconnected, connected, unexpected;
    private boolean bound;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        bound = bindService(new Intent(this, Provider.class), this, Context.BIND_AUTO_CREATE);
        if (!bound) throw new AssertionError("bindService rejected the provider");
        handler.postDelayed(() -> report(false), 16_000);
    }

    private int call(IBinder target, int operation) throws RemoteException {
        Parcel request = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            if (!target.transact(operation, request, reply, 0)) throw new AssertionError("Provider did not answer");
            return reply.readInt();
        } finally {
            reply.recycle();
            request.recycle();
        }
    }

    @Override public void onServiceConnected(ComponentName name, IBinder service) {
        try {
            int pid = call(service, GET_PID);
            connected++;
            Log.i(TAG, "CONNECTED count=" + connected + " pid=" + pid + " client=" + Process.myPid());
            if (connected == 1) {
                initialPid = pid;
                if (pid == Process.myPid()) throw new AssertionError("Provider must be remote");
                if (call(service, EXIT) != pid) throw new AssertionError("Wrong process acknowledged EXIT");
            } else if (connected == 2) {
                handler.postDelayed(() -> report(pid != initialPid && pid != Process.myPid()), 250);
            }
        } catch (RemoteException e) {
            Log.e(TAG, "FAIL unexpected RPC failure", e);
            report(false);
        }
    }

    @Override public void onServiceDisconnected(ComponentName name) {
        Log.i(TAG, "DISCONNECTED count=" + ++disconnected);
    }

    @Override public void onNullBinding(ComponentName name) {
        unexpected++;
        Log.e(TAG, "UNEXPECTED_NULL_BINDING");
    }

    @Override public void onBindingDied(ComponentName name) {
        unexpected++;
        Log.e(TAG, "UNEXPECTED_BINDING_DIED");
    }

    private void report(boolean newRemotePid) {
        if (!bound) return;
        boolean passed = newRemotePid && connected == 2 && disconnected == 1 && unexpected == 0;
        Log.i(TAG, (passed ? "PASS" : "FAIL") + " connected=" + connected
                + " disconnected=" + disconnected + " unexpected=" + unexpected);
        bound = false;
        unbindService(this);
        finish();
    }

    public static final class Provider extends Service {
        @Override public IBinder onBind(Intent intent) {
            Log.i(TAG, "PROVIDER_BIND pid=" + Process.myPid());
            return new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                        throws RemoteException {
                    if (code == GET_PID || code == EXIT) {
                        reply.writeInt(Process.myPid());
                        if (code == EXIT) new Handler(Looper.getMainLooper()).postDelayed(
                                () -> {
                                    Log.i(TAG, "PROVIDER_HALT pid=" + Process.myPid());
                                    Runtime.getRuntime().halt(79);
                                }, 150);
                        return true;
                    }
                    return super.onTransact(code, data, reply, flags);
                }
            };
        }
    }
}
