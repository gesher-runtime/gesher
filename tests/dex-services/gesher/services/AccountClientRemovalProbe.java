/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.IAccountManager;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.os.ServiceManager;
import java.util.Arrays;

/** Host IPC checks of known synthetic accounts in the broad runner's real SystemServer. */
public final class AccountClientRemovalProbe {
    private static final String TYPE = "com.gesher.devsvc.account";
    private static final String CLIENT = "com.gesher.microgprobe";
    private static final Account RETAINED = new Account("retained-account@example.test", TYPE);
    private static final Account VISIBLE = new Account("visible-ungranted@example.test", TYPE);
    private static void require(boolean value, String why) { if (!value) throw new AssertionError(why); }

    private static void providerIntact(IAccountManager accounts) throws Exception {
        Account[] all = accounts.getAccountsAsUser(TYPE, 0, "android");
        require(all != null && all.length == 2 && Arrays.asList(all).contains(RETAINED)
                && Arrays.asList(all).contains(VISIBLE), "Client removal changed the provider's accounts");
        require("secret".equals(accounts.getPassword(RETAINED))
                && "on".equals(accounts.getUserData(RETAINED, "sync"))
                && "imap.example.org".equals(accounts.getUserData(RETAINED, "server"))
                && "fake-persisted-token".equals(accounts.peekAuthToken(RETAINED, "persisted"))
                && "visible-fixture-secret".equals(accounts.getPassword(VISIBLE))
                && "fake-visible-ungranted-token".equals(accounts.peekAuthToken(VISIBLE, "probe-ungranted")),
                "Client removal or restart changed an unrelated provider credential");
    }

    private static void checkOwnerRemoval(IAccountManager accounts, String stage) throws Exception {
        if ("owner-prepare".equals(stage)) {
            require(accounts.addAccountExplicitly(VISIBLE, "owner-fixture-secret", null, "android"),
                    "Could not create a real package-owned fixture account");
            accounts.setAuthToken(VISIBLE, "retirement", "owner-fixture-token");
            require(accounts.setAccountVisibility(VISIBLE, CLIENT, AccountManager.VISIBILITY_VISIBLE),
                    "Could not grant visibility to the retained client");
            require(accounts.getAccountsByTypeForPackage(TYPE, CLIENT, "android").length == 1,
                    "The retained client did not see its granted account");
        } else {
            require(accounts.getAccountsAsUser(TYPE, 0, "android").length == 0
                    && accounts.getAccountsByTypeForPackage(TYPE, CLIENT, "android").length == 0,
                    "A committed provider removal left a visible account");
            require(accounts.getAccountVisibility(VISIBLE, CLIENT) == AccountManager.VISIBILITY_NOT_VISIBLE,
                    "The retained client inherited the discarded visibility grant");
            try {
                accounts.peekAuthToken(VISIBLE, "retirement");
                throw new AssertionError("A retired credential was still readable");
            } catch (SecurityException expected) {
                // An absent account has no credential owner.
            }
        }
        System.out.println("AccountClientRemovalProbe PASS_" + stage);
    }

    public static void main(String[] args) throws Exception {
        require(args.length >= 1, "Pass a fixture stage for the account removal probe");
        System.loadLibrary("android_runtime");
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        IAccountManager accounts = IAccountManager.Stub.asInterface(ServiceManager.getService("account"));
        require(pm != null && accounts != null, "A genuine running SystemServer is required");
        if (args[0].equals("owner-prepare") || args[0].equals("owner-after")
                || args[0].equals("owner-reinstall")) {
            checkOwnerRemoval(accounts, args[0]);
            return;
        }
        require(args[0].equals("prepare") || args[0].equals("after") || args[0].equals("restart"),
                "Unknown account removal probe stage");
        ApplicationInfo installed = pm.getApplicationInfo(CLIENT, 0, 0);
        require(installed != null, "Synthetic client was not installed");
        int uid = installed.uid;
        providerIntact(accounts);
        if (args[0].equals("prepare")) {
            require(args.length == 1, "Unexpected prepare arguments");
            require(accounts.getAccountVisibility(VISIBLE, CLIENT) == AccountManager.VISIBILITY_VISIBLE,
                    "Initial synthetic client visibility was not granted");
            require(accounts.getAccountsByTypeForPackage(TYPE, CLIENT, "android").length == 1,
                    "Initial client account enumeration did not reflect its grant");
            accounts.updateAppPermission(VISIBLE, "probe-ungranted", uid, true);
            System.out.println("AccountClientRemovalProbe READY_GRANTED uid=" + uid);
        } else {
            require(args.length == 2 && uid == Integer.parseInt(args[1]), "Reinstall or restart changed the retained package UID");
            require(accounts.getAccountVisibility(VISIBLE, CLIENT) == AccountManager.VISIBILITY_NOT_VISIBLE,
                    "Same-identity reinstall or restart revived removed client visibility");
            require(accounts.getAccountsByTypeForPackage(TYPE, CLIENT, "android").length == 0,
                    "Same-identity reinstall or restart revived client account enumeration");
            System.out.println("AccountClientRemovalProbe PASS_" + args[0].toUpperCase(java.util.Locale.ROOT)
                    + " uid-stable=true visibility-revoked=true enumeration-empty=true provider-intact=true");
        }
    }
}
