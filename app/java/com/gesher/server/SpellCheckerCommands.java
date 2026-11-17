/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Context;
import android.os.Bundle;
import android.os.Process;
import android.os.ServiceManager;
import android.provider.Settings;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import com.android.internal.textservice.ITextServicesManager;

import java.util.List;

/** Windows launcher's host-only configuration path for the Android text-services manager. */
public final class SpellCheckerCommands {
    private SpellCheckerCommands() {}

    public static int run(List<String> args) throws Exception {
        boolean one = args.size() == 1;
        boolean two = args.size() == 2;
        String action = args.isEmpty() ? "" : args.get(0);
        if (!(one && (action.equals("list") || action.equals("automatic")
                || action.equals("enable") || action.equals("disable"))
                || two && (action.equals("use") || action.equals("language")))) {
            throw new IllegalArgumentException("spell-checker list|use COMPONENT|automatic|enable|disable|language SUBTYPE_ID");
        }
        ITextServicesManager manager = ITextServicesManager.Stub.asInterface(
                ServiceManager.getService(Context.TEXT_SERVICES_MANAGER_SERVICE));
        if (manager == null) throw new IllegalStateException("Text services are unavailable");
        if (action.equals("list")) {
            SpellCheckerInfo current = manager.getCurrentSpellChecker(0, null);
            SpellCheckerInfo[] available = manager.getEnabledSpellCheckers(0);
            System.out.println("Spell checking " + (manager.isSpellCheckerEnabled(0) ? "enabled" : "disabled"));
            if (available == null) return 0;
            for (SpellCheckerInfo checker : available) {
                System.out.println(checker.getId() + (current != null && current.getId().equals(checker.getId())
                        ? " (selected)" : ""));
                for (int i = 0; i < checker.getSubtypeCount(); i++) {
                    SpellCheckerSubtype subtype = checker.getSubtypeAt(i);
                    System.out.println("  " + subtype.hashCode() + " " + subtype.getLocaleObject());
                }
            }
            return 0;
        }
        String key;
        String desired;
        switch (action) {
            case "use":
                ComponentName component = ComponentName.unflattenFromString(args.get(1));
                if (component == null) throw new IllegalArgumentException("Expected PACKAGE/CLASS");
                SpellCheckerInfo[] available = manager.getEnabledSpellCheckers(0);
                boolean found = false;
                if (available != null) {
                    for (SpellCheckerInfo checker : available) {
                        if (component.equals(checker.getComponent())) found = true;
                    }
                }
                if (!found) throw new IllegalArgumentException("No installed spell checker " + args.get(1));
                key = Settings.Secure.SELECTED_SPELL_CHECKER;
                desired = component.flattenToShortString();
                break;
            case "automatic":
                key = Settings.Secure.SELECTED_SPELL_CHECKER;
                desired = "";
                break;
            case "language":
                int id = Integer.parseInt(args.get(1));
                SpellCheckerInfo current = manager.getCurrentSpellChecker(0, null);
                boolean supported = id == 0;
                if (current != null) {
                    for (int i = 0; i < current.getSubtypeCount(); i++) {
                        if (current.getSubtypeAt(i).hashCode() == id) supported = true;
                    }
                }
                if (!supported) throw new IllegalArgumentException("Unknown subtype of the selected checker");
                key = Settings.Secure.SELECTED_SPELL_CHECKER_SUBTYPE;
                desired = Integer.toString(id);
                break;
            default:
                key = Settings.Secure.SPELL_CHECKER_ENABLED;
                desired = action.equals("enable") ? "1" : "0";
                break;
        }
        IActivityManager activity = IActivityManager.Stub.asInterface(ServiceManager.getService(Context.ACTIVITY_SERVICE));
        if (activity == null) throw new IllegalStateException("Activity service unavailable");
        ContentProviderHolder settings = activity.getContentProviderExternal(Settings.AUTHORITY, 0, null, "spell-checker");
        if (settings == null || settings.provider == null) throw new IllegalStateException("Settings provider unavailable");
        AttributionSource source = new AttributionSource(Process.SHELL_UID, "com.android.shell", null);
        Bundle value = new Bundle();
        value.putString("value", desired);
        settings.provider.call(source, Settings.AUTHORITY, "PUT_secure", key, value);
        System.out.println("Spell-checker setting updated");
        return 0;
    }
}
