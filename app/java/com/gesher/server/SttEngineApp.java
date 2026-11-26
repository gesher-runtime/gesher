/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.provider.Settings;

import java.io.File;

/**
 * Gesher Speech Recognition ({@code gesher-stt.apk} next to dalvikvm.exe, app/gesher-stt;
 * docs/speech-recognition.md), the device's RecognitionService over the PC's SAPI
 * recognizers. As {@link CameraApp}: the persistent device installs (or updates) it at its
 * start when Windows has at least one recognizer, and uninstalls it when there are none,
 * as a device without a recognizer has no recognition service
 * (SpeechRecognizer.isRecognitionAvailable answers false).
 *
 * <p>When it is first installed it is granted RECORD_AUDIO, as AOSP's
 * DefaultPermissionGrantPolicy grants MICROPHONE_PERMISSIONS to the system package that
 * handles {@code android.speech.RecognitionService}; a later revocation is kept, as the
 * grant is not repeated for an update or an unchanged package. The calling app's own
 * RECORD_AUDIO is still checked for every session by RecognitionService.
 *
 * <p>Settings.Secure {@code voice_recognition_service} defaults to it (a default row: a
 * value written to the setting, or GESHER_SPEECH_RECOGNIZER's, is kept).
 */
final class SttEngineApp {
    static final String PACKAGE = "com.gesher.stt";
    static final String SERVICE = PACKAGE + "/.WindowsRecognitionService";

    private SttEngineApp() {
    }

    static void sync(Installer installer, PackageManagerService pm) {
        try {
            String exe = Zygote.exePath();
            String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
            // Drive-less, with '/', as the launcher hands APK paths over.
            String apk = (dir + "gesher-stt.apk").replace('\\', '/').replaceFirst("^[A-Za-z]:", "");
            int recognizers = WindowsSpeechRecognition.recognizers().size();
            boolean installed = pm.get(PACKAGE) != null;
            if (recognizers > 0 && new File(apk).isFile()) {
                String[] r = installer.install(apk);
                SystemServer.log("speech recognition: " + PACKAGE + " " + r[1] + " ("
                        + recognizers + " Windows recognizer(s))");
                PackageInstallerService.WINDOWS_SHELL.installed(r[0], r[2],
                        !"unchanged".equals(r[1]));
                if ("installed".equals(r[1])) {
                    pm.permissions().grantRuntime(PACKAGE, Manifest.permission.RECORD_AUDIO, 0);
                }
                SettingsProvider.addSecureDefault(Settings.Secure.VOICE_RECOGNITION_SERVICE, SERVICE);
            } else if (recognizers == 0 && installed) {
                installer.uninstall(PACKAGE);
                SystemServer.log("speech recognition: no Windows recognizer, " + PACKAGE
                        + " uninstalled");
            } else {
                SystemServer.log("speech recognition: " + (recognizers == 0
                        ? "no Windows recognizer" : "no gesher-stt.apk") + ", none installed");
            }
        } catch (Throwable t) {
            SystemServer.log("speech recognition: " + t);
        }
    }
}
