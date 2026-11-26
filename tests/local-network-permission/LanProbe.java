/* SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.lan;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;

public final class LanProbe extends Activity {
    private static final String TAG = "GESHER_LAN_PROBE";
    private static final int TIMEOUT_MS = 2000;
    private static final int REQUEST = 0x47657368;
    private static final int RESPONSE = 0x65725141;

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        String host = getIntent().getStringExtra("host");
        int port = getIntent().getIntExtra("port", 0);
        String label = getIntent().getStringExtra("label");
        int local = checkSelfPermission("android.permission.ACCESS_LOCAL_NETWORK");
        int nearby = checkSelfPermission("android.permission.NEARBY_WIFI_DEVICES");
        Log.i(TAG, label + " local=" + local + " nearby=" + nearby
                + " target=" + getApplicationInfo().targetSdkVersion);
        new Thread(() -> {
            run(label, "tcp", () -> tcp(host, port));
            run(label, "udp", () -> udp(host, port));
            Log.i(TAG, label + " complete");
            runOnUiThread(this::finish);
        }, "lan-probe").start();
    }

    private interface Probe {
        int receive() throws IOException;
    }

    private static void run(String label, String transport, Probe test) {
        try {
            int actual = test.receive();
            Log.i(TAG, label + " " + transport + " "
                    + (actual == RESPONSE ? "echo-ok" : "bad-response-" + actual));
        } catch (IOException | SecurityException ex) {
            Log.i(TAG, label + " " + transport + " " + ex.getClass().getSimpleName()
                    + " " + ex.getMessage());
        }
    }

    private static int tcp(String host, int port) throws IOException {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, port), TIMEOUT_MS);
            socket.setSoTimeout(TIMEOUT_MS);
            DataOutputStream output = new DataOutputStream(socket.getOutputStream());
            output.writeInt(REQUEST);
            output.flush();
            return new DataInputStream(socket.getInputStream()).readInt();
        }
    }

    private static int udp(String host, int port) throws IOException {
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setSoTimeout(TIMEOUT_MS);
            byte[] payload = ByteBuffer.allocate(4).putInt(REQUEST).array();
            socket.send(new DatagramPacket(payload, payload.length,
                    InetAddress.getByName(host), port));
            byte[] response = new byte[4];
            DatagramPacket packet = new DatagramPacket(response, response.length);
            socket.receive(packet);
            if (packet.getLength() != response.length) {
                throw new IOException("unexpected datagram length " + packet.getLength());
            }
            return ByteBuffer.wrap(response).getInt();
        }
    }
}
