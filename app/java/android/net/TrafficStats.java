/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.content.Context;

import java.io.FileDescriptor;
import java.net.DatagramSocket;
import java.net.Socket;

/**
 * Placeholder for the connectivity module's TrafficStats, which framework.jar is
 * compiled against but does not contain (app/README.md, "Classes framework.jar does not
 * have"). ActivityThread.handleBindApplication calls {@link #init}; apps may read the
 * counters. There is no traffic accounting: every counter is {@link #UNSUPPORTED},
 * socket tagging does nothing, and the thread tags are kept per thread. When the
 * module's class is on the boot class path ahead of gesher-app.jar, it wins.
 */
public class TrafficStats {
    public static final int UNSUPPORTED = -1;
    public static final int TAG_NETWORK_STACK_IMPERSONATION_RANGE_END = -113;
    public static final int TAG_NETWORK_STACK_IMPERSONATION_RANGE_START = -128;
    public static final int TAG_NETWORK_STACK_RANGE_END = -257;
    public static final int TAG_NETWORK_STACK_RANGE_START = -768;
    public static final int TAG_SYSTEM_IMPERSONATION_RANGE_END = -241;
    public static final int TAG_SYSTEM_IMPERSONATION_RANGE_START = -256;

    private static final ThreadLocal<int[]> sThread = ThreadLocal.withInitial(
            () -> new int[] {0, -1});  // tag, uid

    public TrafficStats() {
    }

    /** handleBindApplication: the app context; nothing to keep it for. */
    public static void init(Context context) {
    }

    public static void attachSocketTagger() {
    }

    public static void setThreadStatsTag(int tag) {
        sThread.get()[0] = tag;
    }

    public static int getAndSetThreadStatsTag(int tag) {
        int old = sThread.get()[0];
        sThread.get()[0] = tag;
        return old;
    }

    public static int getThreadStatsTag() {
        return sThread.get()[0];
    }

    public static void clearThreadStatsTag() {
        sThread.get()[0] = 0;
    }

    public static void setThreadStatsTagApp() {
        setThreadStatsTag(-251);
    }

    public static void setThreadStatsTagBackup() {
        setThreadStatsTag(-254);
    }

    public static void setThreadStatsTagRestore() {
        setThreadStatsTag(-253);
    }

    public static void setThreadStatsTagDownload() {
        setThreadStatsTag(-252);
    }

    public static void setThreadStatsUid(int uid) {
        sThread.get()[1] = uid;
    }

    public static int getThreadStatsUid() {
        return sThread.get()[1];
    }

    public static void clearThreadStatsUid() {
        sThread.get()[1] = -1;
    }

    public static void tagSocket(Socket socket) {
    }

    public static void untagSocket(Socket socket) {
    }

    public static void tagDatagramSocket(DatagramSocket socket) {
    }

    public static void untagDatagramSocket(DatagramSocket socket) {
    }

    public static void tagFileDescriptor(FileDescriptor fd) {
    }

    public static void untagFileDescriptor(FileDescriptor fd) {
    }

    public static void incrementOperationCount(int operationCount) {
    }

    public static void incrementOperationCount(int tag, int operationCount) {
    }

    public static long getMobileTxPackets() { return UNSUPPORTED; }
    public static long getMobileRxPackets() { return UNSUPPORTED; }
    public static long getMobileTxBytes() { return UNSUPPORTED; }
    public static long getMobileRxBytes() { return UNSUPPORTED; }
    public static long getTxPackets(String iface) { return UNSUPPORTED; }
    public static long getRxPackets(String iface) { return UNSUPPORTED; }
    public static long getTxBytes(String iface) { return UNSUPPORTED; }
    public static long getRxBytes(String iface) { return UNSUPPORTED; }
    public static long getTotalTxPackets() { return UNSUPPORTED; }
    public static long getTotalRxPackets() { return UNSUPPORTED; }
    public static long getTotalTxBytes() { return UNSUPPORTED; }
    public static long getTotalRxBytes() { return UNSUPPORTED; }
    public static long getUidTxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidRxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidTxPackets(int uid) { return UNSUPPORTED; }
    public static long getUidRxPackets(int uid) { return UNSUPPORTED; }
    public static long getUidTcpTxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidTcpRxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidUdpTxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidUdpRxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidTcpTxSegments(int uid) { return UNSUPPORTED; }
    public static long getUidTcpRxSegments(int uid) { return UNSUPPORTED; }
    public static long getUidUdpTxPackets(int uid) { return UNSUPPORTED; }
    public static long getUidUdpRxPackets(int uid) { return UNSUPPORTED; }
}
