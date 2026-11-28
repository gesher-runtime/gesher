/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;

/** Windows supervisor for the packaged standalone DEX command. */
public final class ContainedDex {
    private ContainedDex() {}

    public static void main(String[] args) {
        try {
            System.exit(run(args));
        } catch (Exception failure) {
            System.err.println("gesher: " + failure.getMessage());
            System.exit(1);
        }
    }

    private static int run(String[] args) {
        if (args.length < 4 || !args[0].startsWith("-Xbootclasspath:")
                || !args[1].startsWith("-Ximage:") || args[3].isEmpty()
                || args[3].startsWith("-")) {
            throw new IllegalArgumentException("Invalid contained DEX launch arguments");
        }
        File dex = new File(args[2]).getAbsoluteFile();
        if (!dex.isFile()) throw new IllegalArgumentException("DEX file is unavailable: " + dex);
        System.loadLibrary("android_runtime");
        AppSandbox.prepareStandaloneRuntime();
        String profile = "Gesher.Dex." + UUID.randomUUID();
        String[] capabilities = {AppSandbox.RUNTIME_FILES, AppSandbox.INTERNET_CLIENT,
                AppSandbox.PRIVATE_NETWORK};
        List<String> command = new ArrayList<>(List.of(Zygote.exePath(), args[0], args[1],
                "-Xint", "-Xusejit:false", "-cp", dex.getPath()));
        command.addAll(Arrays.asList(args).subList(3, args.length));
        String line = String.join(" ", command.stream().map(Zygote::quote).toList());
        try (Zygote.StandaloneInput code = new Zygote.StandaloneInput(dex.getPath())) {
            return code.run(line, profile, "Gesher standalone DEX", capabilities,
                    android.os.Process.FIRST_APPLICATION_UID);
        } finally {
            int result = Zygote.nativeDeleteAppContainer(profile);
            if (result != 0) {
                System.err.println("gesher: could not remove DEX container (Win32 error "
                        + result + ")");
            }
        }
    }
}
