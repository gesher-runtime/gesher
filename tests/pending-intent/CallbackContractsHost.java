/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class CallbackContractsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if ("content".equals(args[1])) {
            gesher.services.ContentObserverLifecycleTest.contracts();
            System.out.println("Content observer host all passed: " + gesher.services.ContentObserverLifecycleTest.checks + " checks");
        } else if ("clipboard".equals(args[1])) {
            gesher.services.ClipboardCallbackTest.contracts();
            System.out.println("Clipboard host all passed: " + gesher.services.ClipboardCallbackTest.checks + " checks");
        } else {
            gesher.services.PendingIntentContractTest.contracts(args[1], false);
            System.out.println("PendingIntent host all passed: " + gesher.services.PendingIntentContractTest.checks + " checks");
        }
        System.out.println("Real production and framework Java classes; constructed package/process state, controlled native identity/allocation, and write-size-only Parcel boundary. No host Binder transport, ART or Windows clipboard claim.");
    }
}
