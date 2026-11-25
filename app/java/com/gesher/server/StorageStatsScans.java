/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.LinkedHashMap;
import java.util.Map;

/** Shares only overlapping scans; completed results are never a cache. */
final class StorageStatsScans {
    // Scans run on their requesting Binder threads, without a second worker pool.
    // Bound disk concurrency and retained distinct keys independently of callers.
    static final int MAX_RUNNING = 2;
    static final int MAX_FLIGHTS = 16;

    interface Work {
        long[] run() throws IOException;
    }

    private static final class Flight {
        boolean started;
        boolean done;
        long[] result;
        Throwable failure;
    }

    private final Map<Object, Flight> mFlights = new LinkedHashMap<>();
    private int mRunning;

    long[] execute(Object key, Work work) throws IOException {
        Flight flight;
        boolean owner = false;
        synchronized (this) {
            checkInterrupted();
            for (;;) {
                flight = mFlights.get(key);
                if (flight != null) break;
                if (mFlights.size() < MAX_FLIGHTS) {
                    flight = new Flight();
                    mFlights.put(key, flight);
                    owner = true;
                    break;
                }
                awaitChange();
            }
            if (!owner) {
                // An interrupted joiner leaves the owner and other readers alone.
                while (!flight.done) awaitChange();
                return outcome(flight);
            }
            try {
                while (mRunning >= MAX_RUNNING || firstPending() != flight) awaitChange();
                checkInterrupted();
                flight.started = true;
                ++mRunning;
            } catch (IOException failure) {
                finish(key, flight, null, failure);
                throw failure;
            }
        }

        long[] result = null;
        Throwable failure = null;
        try {
            result = work.run();
        } catch (Throwable caught) {
            failure = caught;
        } finally {
            synchronized (this) {
                finish(key, flight, result, failure);
            }
        }
        return outcome(flight);
    }

    // Called with this monitor held. Pending distinct scans start in admission order.
    private Flight firstPending() {
        for (Flight flight : mFlights.values()) if (!flight.started) return flight;
        return null;
    }

    private void finish(Object key, Flight flight, long[] result, Throwable failure) {
        flight.result = result;
        flight.failure = failure;
        flight.done = true;
        mFlights.remove(key);
        if (flight.started) --mRunning;
        notifyAll();
    }

    private static long[] outcome(Flight flight) throws IOException {
        if (flight.failure instanceof IOException) throw (IOException) flight.failure;
        if (flight.failure instanceof RuntimeException) throw (RuntimeException) flight.failure;
        if (flight.failure instanceof Error) throw (Error) flight.failure;
        if (flight.failure != null) throw new IOException("Storage scan failed", flight.failure);
        return flight.result;
    }

    private void awaitChange() throws InterruptedIOException {
        try {
            wait();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Storage scan interrupted");
        }
    }

    static void checkInterrupted() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Storage scan interrupted");
        }
    }
}
