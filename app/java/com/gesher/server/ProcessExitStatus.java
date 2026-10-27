/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.HashMap;
import java.util.Map;

/** The OS exit result of a package's latest start, not an older process with the same name. */
public final class ProcessExitStatus {
    private static final class Start {
        final long sequence;
        int pid;
        Integer code;

        Start(long sequence) {
            this.sequence = sequence;
        }
    }

    private final Map<String, Start> mStarts = new HashMap<>();

    public synchronized void started(String packageName, long sequence) {
        mStarts.put(packageName, new Start(sequence));
    }

    public synchronized boolean bind(String packageName, long sequence, int pid) {
        Start start = mStarts.get(packageName);
        if (start == null || start.sequence != sequence || pid <= 0
                || (start.pid != 0 && start.pid != pid)) return false;
        start.pid = pid;
        return true;
    }

    public synchronized boolean exited(String packageName, long sequence, int pid, int code) {
        Start start = mStarts.get(packageName);
        if (start == null || start.sequence != sequence || start.pid == 0 || start.pid != pid) {
            return false;
        }
        if (start.code != null) return start.code == code;
        start.code = code;
        return true;
    }

    public synchronized Integer code(String packageName) {
        Start start = mStarts.get(packageName);
        return start != null ? start.code : null;
    }
}
