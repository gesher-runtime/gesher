/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import gesher.services.AudioFocusLifecycleTest;

/** The common fixture uses the real service/framework and controlled Binder lifetime. */
public final class AudioFocusHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        AudioFocusLifecycleTest.contracts(AudioFocusLifecycleTest.service());
        System.out.println("Audio focus host checks: " + AudioFocusLifecycleTest.checks);
        System.out.println("Real service/framework; controlled Binder lifetime and constructor JNI only. No ART, Binder transport, Windows audio enforcement or public-manager runtime claim.");
    }
}
