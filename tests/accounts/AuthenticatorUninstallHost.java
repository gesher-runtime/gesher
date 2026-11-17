/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class AuthenticatorUninstallHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // Official process-local nonce mode; no Android shared-memory mapping on the JVM.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        long started = System.nanoTime();
        gesher.services.AuthenticatorUninstallTest.contracts(args[1]);
        System.out.println("Authenticator uninstall host passed: " + args[1] + "; "
                + gesher.services.AuthenticatorUninstallTest.checks + " checks; "
                + (System.nanoTime() - started) / 1000000 + " ms");
    }
}
