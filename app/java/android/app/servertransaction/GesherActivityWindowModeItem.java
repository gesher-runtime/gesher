/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app.servertransaction;

import android.app.ClientTransactionHandler;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import com.gesher.app.window.WindowMode;
import com.gesher.app.window.WindowSession;

/** Trusted lifecycle metadata, never an app-controlled Intent extra. */
public final class GesherActivityWindowModeItem extends ClientTransactionItem {
    public static final String DESCRIPTOR = "com.gesher.activity-window-mode.v1";
    public static final int RESOLVE = IBinder.FIRST_CALL_TRANSACTION;
    private final IBinder mToken;
    private final IBinder mGrant;
    private final boolean mRevoke;
    private boolean mAuthenticated;

    public GesherActivityWindowModeItem(IBinder token, IBinder grant) {
        if (token == null || grant == null) throw new IllegalArgumentException("Missing activity window capability");
        mToken = token; mGrant = grant; mRevoke = false;
    }
    private GesherActivityWindowModeItem(IBinder token) {
        if (token == null) throw new IllegalArgumentException("Missing activity token");
        mToken = token; mGrant = null; mRevoke = true;
    }
    public static GesherActivityWindowModeItem revoke(IBinder token) {
        return new GesherActivityWindowModeItem(token);
    }
    private GesherActivityWindowModeItem(Parcel in) {
        mToken = in.readStrongBinder(); mGrant = in.readStrongBinder(); mRevoke = in.readBoolean();
        if (mToken == null || !mRevoke && mGrant == null || mRevoke && mGrant != null) {
            throw new IllegalArgumentException("Invalid activity window capability");
        }
    }
    @Override public IBinder getActivityToken() { return mToken; }
    @Override boolean shouldHaveDefinedPreExecutionState() { return false; }
    @Override public void preExecute(ClientTransactionHandler client) {
        if (Binder.getCallingUid() != android.os.Process.SYSTEM_UID) {
            throw new SecurityException("Activity window mode requires system_server");
        }
        mAuthenticated = true;
    }
    @Override public void execute(ClientTransactionHandler client, PendingTransactionActions actions) {
        if (!mAuthenticated) throw new SecurityException("Unauthenticated activity window metadata");
        if (mRevoke) {
            WindowSession session = WindowSession.get();
            if (session != null) session.revokeActivityVisibility(mToken);
            return;
        }
        if (client.getActivitiesToBeDestroyed().containsKey(mToken)
                || client.getActivityClient(mToken) != null) return; // new ViewRoots only
        String mode = resolve(mGrant, mToken);
        if (WindowMode.WINDOW.equals(mode) || WindowMode.HIDDEN.equals(mode)) {
            WindowMode.prepareActivityWindow(mToken, mode);
        }
    }
    public static String resolve(IBinder grant, IBinder token) {
        if (grant == null || token == null) return null;
        Parcel data = Parcel.obtain(); Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR); data.writeStrongBinder(token);
            if (!grant.transact(RESOLVE, data, reply, 0)) return null;
            reply.readException();
            String mode = reply.readString();
            return WindowMode.WINDOW.equals(mode) || WindowMode.HIDDEN.equals(mode) ? mode : null;
        } catch (RemoteException e) { return null; }
        finally { data.recycle(); reply.recycle(); }
    }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeStrongBinder(mToken); dest.writeStrongBinder(mGrant); dest.writeBoolean(mRevoke);
    }
    public static final Parcelable.Creator<GesherActivityWindowModeItem> CREATOR =
            new Parcelable.Creator<>() {
                @Override public GesherActivityWindowModeItem createFromParcel(Parcel in) {
                    return new GesherActivityWindowModeItem(in);
                }
                @Override public GesherActivityWindowModeItem[] newArray(int size) {
                    return new GesherActivityWindowModeItem[size];
                }
            };
}
