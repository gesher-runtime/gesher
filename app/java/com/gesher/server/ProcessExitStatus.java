/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.HashMap;
import java.util.Map;

/** The OS exit result of a package's latest start, not an older process with the same name. */
public final class ProcessExitStatus {
    public enum State { STARTING, RUNNING, FINISHED, UNAVAILABLE }

    public static final class Snapshot {
        public final State state;
        public final boolean hasExitCode;
        public final int exitCode;
        private Snapshot(State state, Integer code) {
            this.state = state; hasExitCode = code != null; exitCode = code == null ? 0 : code;
        }
    }

    /** A retained server-created generation, independent of a later same-name start. */
    public static final class Generation {
        private final ProcessExitStatus owner;
        final long sequence;
        int pid;
        Integer code;
        boolean unavailable;

        private Generation(ProcessExitStatus owner, long sequence) {
            this.owner = owner; this.sequence = sequence;
        }
    }

    private final Map<String, Generation> mStarts = new HashMap<>();

    public synchronized void started(String packageName, long sequence) {
        mStarts.put(packageName, new Generation(this, sequence));
    }

    public synchronized Generation capture(String name, long sequence) {
        Generation start = mStarts.get(name);
        return start != null && start.sequence == sequence ? start : null;
    }

    public synchronized boolean bind(String packageName, long sequence, int pid) {
        Generation start = mStarts.get(packageName);
        if (start == null || start.sequence != sequence) return false;
        return bind(start, pid);
    }

    /** The native starter may return this original PID after cancellation/replacement. */
    public synchronized boolean bind(Generation start, int pid) {
        if (start == null || start.owner != this || pid <= 0 || (start.pid != 0 && start.pid != pid)) return false;
        start.pid = pid;
        return true;
    }

    public synchronized boolean exited(String packageName, long sequence, int pid, int code) {
        Generation start = mStarts.get(packageName);
        if (start == null || start.sequence != sequence || start.pid == 0 || start.pid != pid) {
            return false;
        }
        return exited(start, pid, code);
    }

    /** Records only the actual bound PID's OS result, even after a newer start replaced the map. */
    public synchronized boolean exited(Generation start, int pid, int code) {
        if (start == null || start.owner != this || start.pid <= 0 || start.pid != pid) return false;
        if (start.code != null) return start.code == code;
        start.code = code; start.unavailable = false; return true;
    }

    public synchronized void unavailable(Generation start) {
        if (start != null && start.owner == this && start.code == null) start.unavailable = true;
    }

    public synchronized Snapshot snapshot(Generation start) {
        if (start == null || start.owner != this) return new Snapshot(State.UNAVAILABLE, null);
        State state = start.code != null ? State.FINISHED : start.unavailable ? State.UNAVAILABLE
                : start.pid > 0 ? State.RUNNING : State.STARTING;
        return new Snapshot(state, start.code);
    }

    public synchronized Integer code(String packageName) {
        Generation start = mStarts.get(packageName);
        return start != null ? start.code : null;
    }
}
