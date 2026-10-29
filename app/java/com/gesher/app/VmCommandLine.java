/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

/** Finds a VM's options in its original Windows command line. */
public final class VmCommandLine {
    private VmCommandLine() {}

    public static String beforeMainClass(String command, String mainClass) {
        int at = 0;
        boolean executable = true;
        while (at < command.length()) {
            while (at < command.length() && space(command.charAt(at))) ++at;
            if (at == command.length()) break;
            int start = at;
            boolean quoted = false;
            StringBuilder argument = new StringBuilder();
            while (at < command.length() && (quoted || !space(command.charAt(at)))) {
                int slashes = 0;
                while (at < command.length() && command.charAt(at) == '\\') {
                    ++slashes;
                    ++at;
                }
                if (at < command.length() && command.charAt(at) == '"') {
                    for (int i = 0; i < slashes / 2; ++i) argument.append('\\');
                    if ((slashes & 1) != 0) {
                        argument.append('"');
                    } else if (quoted && at + 1 < command.length()
                            && command.charAt(at + 1) == '"') {
                        argument.append('"');
                        ++at;
                    } else {
                        quoted = !quoted;
                    }
                    ++at;
                } else {
                    for (int i = 0; i < slashes; ++i) argument.append('\\');
                    if (at < command.length() && (quoted || !space(command.charAt(at)))) {
                        argument.append(command.charAt(at++));
                    }
                }
            }
            if (!executable && mainClass.contentEquals(argument)) {
                return command.substring(0, start).stripTrailing();
            }
            executable = false;
        }
        throw new IllegalStateException("no " + mainClass + " in this command line: " + command);
    }

    private static boolean space(char c) { return c == ' ' || c == '\t'; }
}
