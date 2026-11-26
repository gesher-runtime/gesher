/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.provider.Settings;

import java.io.File;

/**
 * Gesher Text-to-speech ({@code gesher-tts.apk} next to dalvikvm.exe, app/gesher-tts;
 * docs/tts.md), the device's TextToSpeechService engine over the PC's Windows voices.
 * As {@link CameraApp}: the persistent device installs (or updates) it at its start when
 * Windows has at least one voice, and uninstalls it when there are none, as a device
 * without voices has no engine and TextToSpeech's onInit reports ERROR.
 *
 * <p>When it is installed, Settings.Secure {@code tts_default_synth} defaults to it
 * (a default row: a value written to the setting is kept). On Android the default engine
 * is otherwise the highest-ranked engine of the system image (TtsEngines
 * .getHighestRankedEngineName, FLAG_SYSTEM engines only); this package is installed by
 * the device rather than part of a system image, so the setting names it.
 * {@code tts_default_locale} is left unset: the engine then speaks the device's locale,
 * as TtsEngines.getLocalePrefForEngine answers without a preference.
 */
final class TtsEngineApp {
    static final String PACKAGE = "com.gesher.tts";

    private TtsEngineApp() {
    }

    static void sync(Installer installer, PackageManagerService pm) {
        try {
            String exe = Zygote.exePath();
            String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
            // Drive-less, with '/', as the launcher hands APK paths over.
            String apk = (dir + "gesher-tts.apk").replace('\\', '/').replaceFirst("^[A-Za-z]:", "");
            int voices = WindowsSpeech.voices().size();
            boolean installed = pm.get(PACKAGE) != null;
            if (voices > 0 && new File(apk).isFile()) {
                String[] r = installer.install(apk);
                SystemServer.log("tts engine: " + PACKAGE + " " + r[1] + " (" + voices
                        + " Windows voice(s))");
                PackageInstallerService.WINDOWS_SHELL.installed(r[0], r[2],
                        !"unchanged".equals(r[1]));
                SettingsProvider.addSecureDefault(Settings.Secure.TTS_DEFAULT_SYNTH, PACKAGE);
            } else if (voices == 0 && installed) {
                installer.uninstall(PACKAGE);
                SystemServer.log("tts engine: no Windows voice, " + PACKAGE + " uninstalled");
            } else {
                SystemServer.log("tts engine: " + (voices == 0 ? "no Windows voice"
                        : "no gesher-tts.apk") + ", none installed");
            }
        } catch (Throwable t) {
            SystemServer.log("tts engine: " + t);
        }
    }
}
