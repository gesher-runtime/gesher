# A small HTTP and HTTPS server on 127.0.0.1 for the WebView tests
# (tests/windows/tracks/37-webview.ps1, docs/webview.md), on threads of the
# test's process:
#   http://127.0.0.1:<HttpPort>/auth   Basic authentication (user "gesher",
#                                      password "secret", realm "gesher-test")
#   http://127.0.0.1:<HttpPort>/post   echoes a POST body in the page title
#   https://127.0.0.1:<HttpsPort>/     a self-signed certificate; asks for a
#                                      client certificate and says whether it
#                                      got one in the page title
#   [GesherWebTestServer]::Start() returns "httpPort httpsPort"; Stop() ends it.
if (-not ('GesherWebTestServer' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
public static class GesherWebTestServer {
  static TcpListener http, https;
  static X509Certificate2 cert;
  public static volatile string Log = "";
  static void Say(string s) { lock (typeof(GesherWebTestServer)) Log += s + "\n"; }

  public static string Start() {
    using (var rsa = RSA.Create(2048)) {
      var req = new CertificateRequest("CN=127.0.0.1", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
      var san = new SubjectAlternativeNameBuilder();
      san.AddIpAddress(IPAddress.Loopback);
      req.CertificateExtensions.Add(san.Build());
      using (var self = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(7))) {
        // (X509CertificateLoader is .NET 9's; pwsh 7.4 runs on .NET 8.)
#pragma warning disable SYSLIB0057
        cert = new X509Certificate2(self.Export(X509ContentType.Pfx, "gesher"), "gesher",
            X509KeyStorageFlags.Exportable);
#pragma warning restore SYSLIB0057
      }
    }
    http = new TcpListener(IPAddress.Loopback, 0); http.Start();
    https = new TcpListener(IPAddress.Loopback, 0); https.Start();
    new Thread(() => Accept(http, false)) { IsBackground = true }.Start();
    new Thread(() => Accept(https, true)) { IsBackground = true }.Start();
    return ((IPEndPoint)http.LocalEndpoint).Port + " " + ((IPEndPoint)https.LocalEndpoint).Port;
  }

  public static void Stop() {
    try { http.Stop(); } catch (Exception) {}
    try { https.Stop(); } catch (Exception) {}
  }

  static void Accept(TcpListener l, bool tls) {
    while (true) {
      TcpClient c;
      try { c = l.AcceptTcpClient(); } catch (Exception) { return; }
      new Thread(() => Serve(c, tls)) { IsBackground = true }.Start();
    }
  }

  static string ReadLine(Stream s) {
    var b = new StringBuilder();
    int ch;
    while ((ch = s.ReadByte()) >= 0) {
      if (ch == '\n') break;
      if (ch != '\r') b.Append((char)ch);
    }
    return ch < 0 && b.Length == 0 ? null : b.ToString();
  }

  static void Serve(TcpClient c, bool tls) {
    try {
      using (c) {
        Stream s = c.GetStream();
        string clientCert = "none";
        if (tls) {
          var ssl = new SslStream(s, false, (o, cc, chain, errors) => true);
          ssl.AuthenticateAsServer(cert, true, SslProtocols.Tls12, false);
          if (ssl.RemoteCertificate != null) clientCert = ssl.RemoteCertificate.Subject;
          s = ssl;
        }
        while (true) {
          string line = ReadLine(s);
          if (string.IsNullOrEmpty(line)) return;
          string[] parts = line.Split(' ');
          string method = parts[0], path = parts.Length > 1 ? parts[1] : "/";
          string auth = null; int length = 0;
          string h;
          while (!string.IsNullOrEmpty(h = ReadLine(s))) {
            int colon = h.IndexOf(':');
            if (colon <= 0) continue;
            string name = h.Substring(0, colon).Trim().ToLowerInvariant(), value = h.Substring(colon + 1).Trim();
            if (name == "authorization") auth = value;
            if (name == "content-length") int.TryParse(value, out length);
          }
          var body = new byte[length];
          for (int read = 0; read < length;) { int n = s.Read(body, read, length - read); if (n <= 0) break; read += n; }
          Say((tls ? "https " : "http ") + method + " " + path + (auth != null ? " (authorization)" : "") + (length > 0 ? " body " + Encoding.UTF8.GetString(body) : ""));
          int status = 200; string extra = ""; string title;
          if (tls) {
            title = "tls client certificate: " + clientCert;
          } else if (path.StartsWith("/auth")) {
            string want = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("gesher:secret"));
            if (auth == want) { title = "auth ok"; }
            else { status = 401; title = "auth needed"; extra = "WWW-Authenticate: Basic realm=\"gesher-test\"\r\n"; }
          } else if (path.StartsWith("/post")) {
            title = method + " " + Encoding.UTF8.GetString(body);
          } else if (path.StartsWith("/favicon")) {
            status = 404; title = "none";
          } else {
            title = "hello";
          }
          byte[] page = Encoding.UTF8.GetBytes("<!doctype html><html><head><title>" + WebUtility.HtmlEncode(title)
              + "</title></head><body style='background:#ffff00'>" + WebUtility.HtmlEncode(title) + "</body></html>");
          string head = "HTTP/1.1 " + status + (status == 200 ? " OK" : status == 401 ? " Unauthorized" : " Not Found")
              + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + page.Length
              + "\r\nCache-Control: no-store\r\n" + extra + "\r\n";
          byte[] hb = Encoding.ASCII.GetBytes(head);
          s.Write(hb, 0, hb.Length); s.Write(page, 0, page.Length); s.Flush();
        }
      }
    } catch (Exception e) {
      Say("error: " + e.GetType().Name + ": " + e.Message);
    }
  }
}
'@
}
