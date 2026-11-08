/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.os.IPowerManager;
import android.os.PowerManager;
import gesher.services.PowerWakeLockCallbackTest;
import gesher.services.PowerWakeLockCallbackTest.Callback;
import gesher.services.PowerWakeLockCallbackTest.Token;
import static gesher.services.PowerWakeLockCallbackTest.check;

/** Host boundary observes real production WindowsHost calls; it cannot keep Windows awake. */
public final class PowerWakeLockHost {
    private static native void reset();
    private static native void failNext();
    private static native int calls();
    private static native boolean system();
    private static native boolean display();
    private static native String reason();
    private static void backend(IPowerManager service) throws Exception {
        reset(); failNext();
        Token token = new Token(); Callback callback = new Callback(service);
        service.acquireWakeLock(token.binder, PowerManager.SCREEN_BRIGHT_WAKE_LOCK, "screen", "gesher.power.fixture", null, null, 0, callback);
        callback.event(true);
        check(calls() == 1 && system() && display(), "Screen lock did not call the real backend boundary");
        service.releaseWakeLock(token.binder, 0); callback.event(false);
        check(calls() == 2 && !system() && !display() && reason().isEmpty(), "Partial backend failure skipped release cleanup");
        check(PowerWakeLockCallbackTest.held(service) == 0 && token.recipients.isEmpty(), "Backend cleanup retained owner");

        reset(); Token retry = new Token(); Callback listener = new Callback(service);
        PowerWakeLockCallbackTest.acquire(service, retry, "original", listener); listener.event(true);
        failNext(); PowerWakeLockCallbackTest.acquire(service, retry, "replacement", listener);
        listener.event(false); listener.event(true);
        check(calls() == 2, "Replacement did not attempt backend update");
        PowerWakeLockCallbackTest.acquire(service, retry, "replacement", listener); listener.empty(service);
        check(calls() == 3, "Failed unchanged request was not retried");
        PowerWakeLockCallbackTest.acquire(service, retry, "replacement", listener); listener.empty(service);
        check(calls() == 3, "Successful unchanged state redundantly reapplied");
        service.releaseWakeLock(retry.binder, 0); listener.event(false);
        check(calls() == 4 && !system() && !display(), "Successful retry did not clear host request");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        IPowerManager service = PowerWakeLockCallbackTest.service();
        PowerWakeLockCallbackTest.contracts(service); backend(service);
        System.out.println("Power wake-lock host checks: " + PowerWakeLockCallbackTest.checks);
        System.out.println("Real service/framework classes; controlled token lifetime and observed native boundary; no Windows power-policy or Binder transport claim.");
    }
}
