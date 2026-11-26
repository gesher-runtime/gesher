/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.DownloadManager;
import com.gesher.net.WindowsNetworks;
import java.nio.file.Files;
import java.nio.file.Path;

/** Native policy samples -> real snapshot parser -> production download constraints.
 * The declared Environment supplies the parsed default network just as
 * DownloadProvider.QueueEnvironment does. No HTTP transfer or Windows JNI is claimed.
 */
public final class UnknownCostDownloadsHost {
    private static int checks;
    private static int failures;

    private static void check(boolean condition, String description) {
        checks++;
        if (!condition) failures++;
        System.out.println((condition ? "ok   " : "FAIL ") + description);
    }

    private static final class Environment implements DownloadQueue.Environment {
        WindowsNetworks.Net current;

        @Override public DownloadQueue.NetworkState network() {
            if (current == null || !current.internet) return null;
            return new DownloadQueue.NetworkState(current.metered, current.roaming);
        }
        @Override public long maxBytesOverMobile() { return Long.MAX_VALUE; }
        @Override public long recommendedMaxBytesOverMobile() { return Long.MAX_VALUE; }
        @Override public DownloadQueue.Transport transport(DownloadRequests.Info info) {
            throw new AssertionError("This constraint test must not open a transfer");
        }
        @Override public String userAgent() { return "cost-contract-test"; }
        @Override public long currentTimeMillis() { return 0; }
        @Override public long elapsedRealtime() { return 0; }
        @Override public int[] powerStatus() { return null; }
        @Override public long idleMillis() { return DeviceConstraints.NOT_OFF; }
        @Override public void listen(Runnable listener) { }
    }

    public static void main(String[] args) throws Exception {
        Environment environment = new Environment();
        DownloadQueue queue = new DownloadQueue(null, environment);
        DownloadRequests.Info request = new DownloadRequests.Info();
        request.isPublicApi = true;
        request.allowedNetworkTypes = DownloadManager.Request.NETWORK_MOBILE
                | DownloadManager.Request.NETWORK_WIFI;
        request.allowRoaming = true;
        int samples = 0;
        try {
            for (String line : Files.readAllLines(Path.of(args[0]))) {
                if (!line.startsWith("sample\t")) continue;
                String[] fields = line.split("\t");
                String label = fields[1];
                // The native policy fixture emits its actual production state;
                // only the unrelated interface/address fields are synthetic.
                String snapshot = "internet\t1\nnet\t7\tethernet\t1500\t1000000\t1000000\t"
                        + fields[2] + "\t" + fields[3] + "\t" + fields[4]
                        + "\t1\t1\tfixture\tfixture\n";
                environment.current = WindowsNetworks.parse(snapshot).defaultNetwork();
                check(environment.current != null && environment.current.internet,
                        label + ": real parser preserves the connected default network");
                boolean unrestricted = label.equals("unrestricted")
                        || label.equals("restored-unrestricted");
                request.allowMetered = false;
                check((queue.networkRefusal(request, 1024) == null) == unrestricted,
                        label + ": unmetered-only download follows the native cost evidence");
                request.allowMetered = true;
                check(queue.networkRefusal(request, 1024) == null,
                        label + ": allowing metered data still permits the connected network");
                samples++;
            }
        } finally {
            queue.shutdown();
        }
        check(samples == 10, "all ten native cost samples reached the download consumer");
        System.out.println("cost-downloads " + (failures == 0 ? "PASSED" : "FAILED")
                + ": " + checks + " checks, " + failures + " failures");
        if (failures != 0) throw new AssertionError("Unknown cost admitted an unmetered-only download");
    }
}
