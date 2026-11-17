/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import static android.view.inputmethod.ConnectionlessHandwritingCallback.CONNECTIONLESS_HANDWRITING_ERROR_UNSUPPORTED;

import android.os.Binder;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.inputmethod.IConnectionlessHandwritingCallback;
import com.android.internal.view.IInputMethodManager;
import com.gesher.server.InputMethodManagerService;
import com.gesher.server.StandIn;

/**
 * Unsupported handwriting completion through the production service. --host uses direct
 * callbacks without native adapters; Windows additionally exercises generated service and
 * callback Parcel dispatch within one process, not real cross-process Binder delivery.
 * AOSP contract: frameworks/base 067b33e5, InputMethodManagerService
 * startConnectionlessStylusHandwriting's unsupported-capability branch.
 */
public final class HandwritingUnavailableContractTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static final class Recording extends IConnectionlessHandwritingCallback.Default {
        int errors, results, error = -1;
        RemoteException failure;

        @Override public void onResult(CharSequence text) { results++; }
        @Override public void onError(int code) throws RemoteException {
            errors++;
            error = code;
            if (failure != null) throw failure;
        }

        void unsupportedOnce() {
            check(errors == 1, "Request must complete with exactly one error callback");
            check(error == CONNECTIONLESS_HANDWRITING_ERROR_UNSUPPORTED,
                    "Unavailable IME must report the Android unsupported error");
            check(results == 0, "Unavailable IME must not fabricate recognized text");
        }
    }

    private static void request(IInputMethodManager service, boolean delegated,
            IConnectionlessHandwritingCallback callback) throws RemoteException {
        // AOSP checks capability before client/delegation validation. Cursor information
        // is nullable; absent support cannot depend on an editor, stylus or host window.
        service.startConnectionlessStylusHandwriting(null, 0, null,
                delegated ? "fixture.handwriting.delegate" : null,
                delegated ? "fixture.handwriting.delegator" : null, callback);
    }

    private static void directContracts(InputMethodManagerService service) throws Exception {
        check(!service.isStylusHandwritingAvailableAsUser(0, true),
                "Connectionless handwriting must remain unavailable");
        check(!service.isStylusHandwritingAvailableAsUser(0, false),
                "No connected handwriting support may be invented");
        for (boolean delegated : new boolean[] {false, true}) {
            Recording first = new Recording();
            request(service, delegated, first);
            first.unsupportedOnce();

            Recording failed = new Recording();
            failed.failure = new DeadObjectException("fixture callback is gone");
            try {
                request(service, delegated, failed);
                throw new AssertionError("Callback RemoteException must propagate as a runtime failure");
            } catch (RuntimeException failure) {
                check(failure.getCause() == failed.failure,
                        "Runtime failure must retain the original callback RemoteException");
            }
            failed.unsupportedOnce();

            Recording next = new Recording();
            request(service, delegated, next);
            next.unsupportedOnce();
            failed.unsupportedOnce(); // No retry or retained callback when another request arrives.
        }
    }

    private static final class Transport extends Binder {
        final IBinder target;
        int transactions, flags;

        Transport(IBinder target) { this.target = target; }
        @Override public IInterface queryLocalInterface(String descriptor) { return null; }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            transactions++;
            this.flags = flags;
            return target.transact(code, data, reply, flags);
        }
    }

    private static void parcelContracts(InputMethodManagerService implementation) throws Exception {
        IBinder target = StandIn.of(IInputMethodManager.class, implementation).asBinder();
        check(target.getClass().getName().equals("com.gesher.server.stub.IInputMethodManagerStub"),
                "Production generated input-method service stub is required");
        Transport transport = new Transport(target);
        IInputMethodManager service = IInputMethodManager.Stub.asInterface(transport);
        check(service.asBinder() == transport, "Service must use the forced generated proxy");
        check(!service.isStylusHandwritingAvailableAsUser(0, true),
                "Capability query must stay unavailable through Binder dispatch");
        for (boolean delegated : new boolean[] {false, true}) {
            Recording recording = new Recording();
            IConnectionlessHandwritingCallback.Stub receiver =
                    new IConnectionlessHandwritingCallback.Stub() {
                        @Override public void onResult(CharSequence text) { recording.onResult(text); }
                        @Override public void onError(int code) throws RemoteException {
                            recording.onError(code);
                        }
                    };
            Transport callbackTransport = new Transport(receiver);
            IConnectionlessHandwritingCallback callback =
                    IConnectionlessHandwritingCallback.Stub.asInterface(callbackTransport);
            check(callback.asBinder() == callbackTransport,
                    "Callback must use the forced generated proxy");
            request(service, delegated, callback);
            recording.unsupportedOnce();
            check(callbackTransport.transactions == 1, "Exactly one callback Parcel must be delivered");
            check((callbackTransport.flags & IBinder.FLAG_ONEWAY) != 0,
                    "Generated callback must preserve its one-way AIDL contract");
        }
        check(transport.transactions == 3,
                "Capability query and both requests must traverse generated service dispatch");
    }

    public static void main(String[] args) throws Exception {
        boolean binder = args.length == 0;
        if (!binder && (args.length != 1 || !"--host".equals(args[0]))) {
            throw new IllegalArgumentException("Use no argument for Windows ART, or --host for direct Java");
        }
        if (binder) System.loadLibrary("android_runtime");
        InputMethodManagerService service = new InputMethodManagerService();
        directContracts(service);
        if (binder) parcelContracts(service);
        System.out.println("HandwritingUnavailableContractTest all passed: " + checks
                + "; binder=" + binder);
        if (binder) System.exit(0);
    }
}
