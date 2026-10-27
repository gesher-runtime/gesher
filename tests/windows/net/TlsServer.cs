// Local TLS fixture only. Own the accepted sockets as well as the listener:
// stopping a TcpListener does not stop an in-flight SChannel handshake/read.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public sealed class GesherNetTlsServer : IDisposable {
    private readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
    private readonly ConcurrentDictionary<TcpClient, byte> clients = new ConcurrentDictionary<TcpClient, byte>();
    private readonly ConcurrentBag<Task> workers = new ConcurrentBag<Task>();
    private readonly X509Certificate2 certificate;
    private readonly IReadOnlyDictionary<string, X509Certificate2> virtualHosts;
    private readonly ConcurrentQueue<string> serverNames = new ConcurrentQueue<string>();
    private readonly ConcurrentQueue<string> certificateSelections = new ConcurrentQueue<string>();
    private readonly Task accept;
    private int stopped;
    public int Port { get; private set; }
    public int ActiveClients { get { return clients.Count; } }

    public string[] SeenServerNames { get { return serverNames.ToArray(); } }
    public string[] CertificateSelections { get { return certificateSelections.ToArray(); } }

    public GesherNetTlsServer(X509Certificate2 certificate) : this(certificate, null) { }

    public GesherNetTlsServer(X509Certificate2 certificate,
                             IReadOnlyDictionary<string, X509Certificate2> virtualHosts) {
        this.certificate = certificate;
        this.virtualHosts = virtualHosts;
        listener.Start();
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        accept = Accept();
    }

    private async Task Accept() {
        try {
            while (Volatile.Read(ref stopped) == 0) {
                TcpClient client = await listener.AcceptTcpClientAsync(cancellation.Token).ConfigureAwait(false);
                if (Volatile.Read(ref stopped) != 0) { client.Dispose(); break; }
                clients.TryAdd(client, 0);
                workers.Add(Serve(client));
            }
        } catch (Exception error) when (Volatile.Read(ref stopped) != 0 &&
                    (error is SocketException || error is ObjectDisposedException || error is OperationCanceledException)) { }
    }

    private async Task Serve(TcpClient client) {
        using (client)
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token)) {
            timeout.CancelAfter(TimeSpan.FromSeconds(20));
            try {
                using (var ssl = new SslStream(client.GetStream(), false)) {
                    var authentication = new SslServerAuthenticationOptions {
                        ClientCertificateRequired = false,
                        EnabledSslProtocols = SslProtocols.Tls12,
                        CertificateRevocationCheckMode = X509RevocationMode.NoCheck
                    };
                    if (virtualHosts == null) {
                        authentication.ServerCertificate = certificate;
                    } else {
                        authentication.ServerCertificateSelectionCallback = (_, name) => {
                            string host = name ?? "";
                            X509Certificate2 selected;
                            if (!virtualHosts.TryGetValue(host, out selected)) selected = certificate;
                            serverNames.Enqueue(host);
                            certificateSelections.Enqueue(host + " => " + selected.Thumbprint);
                            return selected;
                        };
                    }
                    await ssl.AuthenticateAsServerAsync(authentication, timeout.Token).ConfigureAwait(false);
                    string header = "";
                    byte[] one = new byte[1];
                    while (!header.EndsWith("\r\n\r\n", StringComparison.Ordinal)) {
                        int count = await ssl.ReadAsync(one.AsMemory(), timeout.Token).ConfigureAwait(false);
                        if (count == 0) return;
                        header += (char)one[0];
                        if (header.Length > 16384) return;
                    }
                    byte[] body = Encoding.ASCII.GetBytes("trusted through the Windows ROOT store");
                    byte[] reply = Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                            body.Length + "\r\nConnection: close\r\n\r\n");
                    await ssl.WriteAsync(reply.AsMemory(), timeout.Token).ConfigureAwait(false);
                    await ssl.WriteAsync(body.AsMemory(), timeout.Token).ConfigureAwait(false);
                    await ssl.FlushAsync(timeout.Token).ConfigureAwait(false);
                }
            } catch (Exception error) when (error is IOException || error is AuthenticationException ||
                        error is OperationCanceledException || error is ObjectDisposedException || error is SocketException) {
                // Rejected certificates and interrupted fixture clients are expected.
            } finally {
                byte ignored;
                clients.TryRemove(client, out ignored);
            }
        }
    }

    public void Dispose() {
        if (Interlocked.Exchange(ref stopped, 1) != 0) return;
        listener.Stop();
        foreach (TcpClient client in clients.Keys) client.Dispose();
        cancellation.Cancel();
        if (!accept.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("TLS fixture accept loop did not stop");
        if (!Task.WaitAll(workers.ToArray(), TimeSpan.FromSeconds(5))) throw new TimeoutException("TLS fixture clients did not stop");
        cancellation.Dispose();
    }
}

public static class GesherNetTlsServerFixture {
    private static void Connected(GesherNetTlsServer server) {
        DateTime deadline = DateTime.UtcNow.AddSeconds(3);
        while (server.ActiveClients == 0 && DateTime.UtcNow < deadline) Thread.Sleep(10);
        if (server.ActiveClients != 1) throw new Exception("The fixture did not accept the client");
    }
    public static void Test(X509Certificate2 certificate) {
        using (var idle = new GesherNetTlsServer(certificate)) { }
        using (var stalled = new GesherNetTlsServer(certificate))
        using (var client = new TcpClient("127.0.0.1", stalled.Port)) {
            Connected(stalled);
            client.GetStream().WriteByte(0x16); // Incomplete TLS record, no completed handshake.
            stalled.Dispose();
            if (stalled.ActiveClients != 0) throw new Exception("Stalled TLS client survived cleanup");
        }
        using (var reading = new GesherNetTlsServer(certificate))
        using (var client = new TcpClient("127.0.0.1", reading.Port))
        // This fixture trusts only its own ephemeral local server; runtime TLS tests
        // still use Conscrypt's real trust and certificate checks.
        using (var ssl = new SslStream(client.GetStream(), false, (sender, cert, chain, errors) => true)) {
            if (!ssl.AuthenticateAsClientAsync("localhost").Wait(TimeSpan.FromSeconds(10)))
                throw new TimeoutException("Fixture client handshake did not finish");
            Connected(reading);
            reading.Dispose(); // Peer sends no HTTP request: cancel the accepted read.
            if (reading.ActiveClients != 0) throw new Exception("TLS reader survived cleanup");
        }
    }
}
