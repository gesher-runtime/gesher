/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNsdResolver;
import com.gesher.net.WindowsNsdResolver.Interface;
import com.gesher.net.WindowsNsdResolver.Packet;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;

/** Real UDP, with a loopback-only test driver and a separate Python wire producer. */
public final class NsdResolvePeerTest {
    private static int checks;
    public static int verificationCount() { return checks; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    public static void verify(NsdServiceInfo info, String type, String instance,
            String hostname, int port, InetAddress address) throws Exception {
        check(info != null, "independent peer resolved");
        check(info.getServiceName().equals(instance) && info.getServiceType().equals("." + type), "instance/type identity");
        check(info.getPort() == port && info.getHostname().equals(hostname), "actual service port/hostname");
        check(info.getHostAddresses().size() == 1
                && Arrays.equals(info.getHostAddresses().get(0).getAddress(), address.getAddress()), "actual peer address");
        check(info.getAttributes().containsKey("flag") && info.getAttributes().get("flag") == null, "wire key-only TXT");
        check(info.getAttributes().containsKey("empty") && info.getAttributes().get("empty").length == 0, "wire empty TXT");
        byte[] expected = new byte[205]; expected[1] = -1; expected[2] = -2; expected[3] = '='; expected[4] = -128;
        for (int at = 0; at < 200; at++) expected[at + 5] = (byte) at;
        check(Arrays.equals(info.getAttributes().get("bin"), expected), "wire opaque binary TXT");
        check(Arrays.equals(info.getAttributes().get("dup"), "first".getBytes(StandardCharsets.US_ASCII)), "wire duplicate key");
        check(Arrays.equals(info.getAttributes().get("replace"), "later".getBytes(StandardCharsets.US_ASCII)), "wire null duplicate exception");
        check(info.getAttributes().size() == 9, "wire distinct entries");
        for (int at = 0; at < 4; at++) {
            byte[] value = info.getAttributes().get("p" + at);
            check(value.length == 230 && value[0] == 65 + at && value[229] == 65 + at, "wire large TXT " + at);
        }
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(info.getHostAddresses().get(0), info.getPort()), 3000);
            socket.setSoTimeout(3000);
            byte[] greeting = new byte[22]; int read = 0;
            while (read < greeting.length) {
                int amount = socket.getInputStream().read(greeting, read, greeting.length - read);
                if (amount < 0) break;
                read += amount;
            }
            check(new String(greeting, 0, read, StandardCharsets.US_ASCII).equals("gesher-owned-nsd-peer\n"), "resolved endpoint reaches the separate real TCP peer");
        }
    }

    private static final class LoopbackDriver implements WindowsNsdResolver.Driver {
        final DatagramSocket socket;
        final InetAddress address;
        final int port, family;
        final Interface scope;
        final long deadline = System.nanoTime() + 10_000_000_000L;
        int received, sent; boolean closed;
        LoopbackDriver(InetAddress address, int port, int family) throws Exception {
            if (!address.isLoopbackAddress()) throw new IllegalArgumentException("Explicit host fixture is loopback only");
            this.address = address; this.port = port; this.family = family;
            int index = NetworkInterface.getByInetAddress(address).getIndex();
            scope = new Interface(index, family == 4 ? index : 0, family == 6 ? index : 0,
                    0, 1, family == 4 ? 1 : 2, 0);
            socket = new DatagramSocket(new InetSocketAddress(address, 0));
        }
        public Interface[] interfaces() { return new Interface[] {scope}; }
        public long generation() { return 1; }
        public boolean configure(Interface[] selected) { return selected.length == 1 && selected[0].equals(scope); }
        public boolean send(int index, byte[] query) throws IOException {
            if (index != scope.index) throw new IOException("Wrong fixture scope");
            socket.send(new DatagramPacket(query, query.length, address, port)); sent++; return true;
        }
        public Packet poll(int timeout) throws IOException {
            if (System.nanoTime() > deadline) throw new IOException("Host fixture deadline; not a production resolution timeout");
            socket.setSoTimeout(Math.max(1, timeout));
            DatagramPacket packet = new DatagramPacket(new byte[65535], 65535);
            try { socket.receive(packet); }
            catch (SocketTimeoutException pending) { return null; }
            if (!packet.getAddress().equals(address) || packet.getPort() != port) return null;
            received++;
            return new Packet(1, scope.index, family, scope.index,
                    Arrays.copyOf(packet.getData(), packet.getLength()));
        }
        public boolean close() { socket.close(); closed = socket.isClosed(); return closed; }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 5) throw new IllegalArgumentException("host initialization library, control, family, type, instance");
        System.load(args[0]);
        Path control = Path.of(args[1]); int family = Integer.parseInt(args[2]);
        List<String> ready = Files.readAllLines(control.resolve("ready"));
        InetAddress address = InetAddress.getByName(family == 4 ? "127.0.0.1" : "::1");
        LoopbackDriver driver = new LoopbackDriver(address, Integer.parseInt(ready.get(0)), family);
        NsdServiceInfo request = new NsdServiceInfo(); request.setServiceType(args[3]); request.setServiceName(args[4]);
        NsdServiceInfo[] result = new NsdServiceInfo[1]; int[] callbacks = {0};
        new WindowsNsdResolver.Resolver(() -> driver, request, new WindowsNsdResolver.Callback() {
            public void resolved(NsdServiceInfo info) { check(driver.closed, "transport closed before real-peer success"); result[0] = info; callbacks[0]++; }
            public void failed(int error) { throw new AssertionError("Real peer resolution failed " + error); }
            public void stopped() { throw new AssertionError("Unexpected stop"); }
            public void stopFailed(int error) { throw new AssertionError("Unexpected stop failure"); }
        }, () -> System.nanoTime() / 1000000L, () -> 0).run();
        check(callbacks[0] == 1 && driver.sent > 0 && driver.received > 0, "actual UDP exchange and one terminal callback");
        verify(result[0], args[3], args[4], ready.get(2), Integer.parseInt(ready.get(1)), address);
        System.out.println("NsdResolvePeerTest all passed: " + checks + "; family=" + family
                + "; transport=loopback-fixture; windowsExecution=false; udpQueries=" + driver.sent
                + "; udpResponses=" + driver.received);
    }
}
