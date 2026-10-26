# The web3 track's test servers and Windows proxy settings (tests/windows/tracks/54-web3.ps1,
# ADR 0058), on threads of the test's process:
#   [GesherWeb3Servers]::Start() -> "proxyPort httpPort"
#     proxy (127.0.0.1:proxyPort): an HTTP proxy. Requests for http://web3-proxied.test/ and
#       http://pac-proxied.test/ (names nothing else resolves) are answered by the proxy
#       itself; every other request is forwarded (absolute URIs) or tunnelled (CONNECT), so
#       whatever else uses the proxy while Windows' settings point to it keeps working.
#     http (127.0.0.1:httpPort): /proxy.pac (a PAC script proxying pac-proxied.test only)
#       and /direct.
#   [GesherWeb3Servers]::Log: what they saw, one line each.
#   [GesherWeb3Proxy]::Save(), SetServer(server, bypass), SetPac(url), Restore(): the
#     user's Internet Options (WinINet's per-connection options, which WinHTTP reads).
if (-not ('GesherWeb3Servers' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

public static class GesherWeb3Servers {
  static TcpListener proxy, http;
  public static int ProxyPort, HttpPort;
  static readonly StringBuilder log = new StringBuilder();
  public static string Log { get { lock (log) return log.ToString(); } }
  static void Say(string s) { lock (log) log.Append(s).Append('\n'); }

  public static string Start() {
    proxy = new TcpListener(IPAddress.Loopback, 0); proxy.Start();
    http = new TcpListener(IPAddress.Any, 0); http.Start();
    ProxyPort = ((IPEndPoint)proxy.LocalEndpoint).Port;
    HttpPort = ((IPEndPoint)http.LocalEndpoint).Port;
    new Thread(() => Accept(proxy, true)) { IsBackground = true }.Start();
    new Thread(() => Accept(http, false)) { IsBackground = true }.Start();
    return ProxyPort + " " + HttpPort;
  }

  public static void Stop() {
    try { proxy.Stop(); } catch (Exception) {}
    try { http.Stop(); } catch (Exception) {}
  }

  static void Accept(TcpListener l, bool isProxy) {
    while (true) {
      TcpClient c;
      try { c = l.AcceptTcpClient(); } catch (Exception) { return; }
      new Thread(() => { if (isProxy) Proxy(c); else Serve(c); }) { IsBackground = true }.Start();
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

  static void Reply(Stream s, int status, string type, string body) {
    byte[] b = Encoding.UTF8.GetBytes(body);
    byte[] h = Encoding.ASCII.GetBytes("HTTP/1.1 " + status + (status == 200 ? " OK" : " Not Found")
        + "\r\nContent-Type: " + type + "\r\nContent-Length: " + b.Length
        + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    s.Write(h, 0, h.Length); s.Write(b, 0, b.Length); s.Flush();
  }

  const string CompatPage = "<!DOCTYPE html><html><head><title>compat</title></head><body>"
      + "<p>compat, through the test proxy</p><script>"
      + "web3.onmessage = function(e) { if (e.data instanceof ArrayBuffer) {"
      + " window.binaryReplyOk = Array.from(new Uint8Array(e.data)).join(',') === '0,255,128,1'; return; }"
      + " document.title = 'got ' + e.data; };"
      + "window.addEventListener('message', function(e) { if (e.data instanceof ArrayBuffer) {"
      + " var bytes = new Uint8Array(e.data); window.binaryMainOk = bytes.length === 70000 &&"
      + " bytes.every(function(value, i) { return value === ((i * 31) & 255); });"
      + " var port = e.ports[0]; port.onmessage = function(event) {"
      + " if (!(event.data instanceof ArrayBuffer)) return;"
      + " if (event.data.byteLength === 0) window.binaryEmptyOk = true;"
      + " else window.binaryPortOk = Array.from(new Uint8Array(event.data)).join(',') === '6,0,255'; };"
      + " port.postMessage(new Uint8Array([5,0,128,255]).buffer); web3.postMessage(e.data); return; }"
      + " window.lastWindowMessage = String(e.data);"
      + " web3.postMessage('window got ' + e.data); });"
      + "web3.postMessage('hello ' + window.__web3Early);"
      + "</script></body></html>";

  static void Proxy(TcpClient c) {
    try {
      using (c) {
        Stream s = c.GetStream();
        string line = ReadLine(s);
        if (string.IsNullOrEmpty(line)) return;
        var headers = new StringBuilder();
        int length = 0;
        string h;
        while (!string.IsNullOrEmpty(h = ReadLine(s))) {
          string lower = h.ToLowerInvariant();
          if (lower.StartsWith("content-length:")) int.TryParse(h.Substring(15).Trim(), out length);
          if (lower.StartsWith("proxy-connection:") || lower.StartsWith("connection:")) continue;
          headers.Append(h).Append("\r\n");
        }
        string[] parts = line.Split(' ');
        string method = parts[0], target = parts.Length > 1 ? parts[1] : "";
        Say("proxy " + method + " " + target);
        if (method == "CONNECT") {
          string[] hp = target.Split(':');
          using (var up = new TcpClient(hp[0], int.Parse(hp[1]))) {
            byte[] ok = Encoding.ASCII.GetBytes("HTTP/1.1 200 Connection established\r\n\r\n");
            s.Write(ok, 0, ok.Length); s.Flush();
            Pipe(s, up.GetStream());
          }
          return;
        }
        var uri = new Uri(target);
        if (uri.Host == "web3-proxied.test" || uri.Host == "pac-proxied.test") {
          if (uri.AbsolutePath.StartsWith("/compat")) Reply(s, 200, "text/html; charset=utf-8", CompatPage);
          else if (uri.AbsolutePath.StartsWith("/favicon")) Reply(s, 404, "text/plain", "none");
          else Reply(s, 200, "text/plain", "proxied by the test proxy for " + uri.Host);
          return;
        }
        var body = new byte[length];
        for (int read = 0; read < length;) { int n = s.Read(body, read, length - read); if (n <= 0) break; read += n; }
        using (var up = new TcpClient(uri.Host, uri.Port)) {
          Stream u = up.GetStream();
          byte[] req = Encoding.ASCII.GetBytes(method + " " + uri.PathAndQuery + " HTTP/1.1\r\n"
              + headers + "Connection: close\r\n\r\n");
          u.Write(req, 0, req.Length); u.Write(body, 0, body.Length); u.Flush();
          u.CopyTo(s);
          s.Flush();
        }
      }
    } catch (Exception e) {
      Say("proxy error: " + e.GetType().Name + ": " + e.Message);
    }
  }

  static void Pipe(Stream a, Stream b) {
    var t = new Thread(() => { try { b.CopyTo(a); } catch (Exception) {} try { a.Close(); } catch (Exception) {} }) { IsBackground = true };
    t.Start();
    try { a.CopyTo(b); } catch (Exception) {}
    try { b.Close(); } catch (Exception) {}
    t.Join(5000);
  }

  static void Serve(TcpClient c) {
    try {
      using (c) {
        Stream s = c.GetStream();
        string line = ReadLine(s);
        if (string.IsNullOrEmpty(line)) return;
        string h;
        while (!string.IsNullOrEmpty(h = ReadLine(s))) { }
        string[] parts = line.Split(' ');
        string path = parts.Length > 1 ? parts[1] : "/";
        Say("http " + parts[0] + " " + path);
        if (path.StartsWith("/proxy.pac")) {
          Reply(s, 200, "application/x-ns-proxy-autoconfig",
              "function FindProxyForURL(url, host) {\n"
              + "  if (host == 'pac-proxied.test') return 'PROXY 127.0.0.1:" + ProxyPort + "';\n"
              + "  return 'DIRECT';\n}\n");
        } else if (path.StartsWith("/network-binding")) {
          Reply(s, 200, "text/plain", ((IPEndPoint)c.Client.RemoteEndPoint).Address.ToString());
        } else if (path.StartsWith("/direct")) {
          Reply(s, 200, "text/plain", "direct from the test server");
        } else {
          Reply(s, 404, "text/plain", "none");
        }
      }
    } catch (Exception e) {
      Say("http error: " + e.GetType().Name + ": " + e.Message);
    }
  }
}

public static class GesherWeb3Proxy {
  [StructLayout(LayoutKind.Sequential)] struct Option { public int dwOption; public int pad; public IntPtr value; }
  [StructLayout(LayoutKind.Sequential)] struct OptionList {
    public int dwSize; public IntPtr pszConnection; public int dwOptionCount; public int dwOptionError; public IntPtr pOptions; }
  [DllImport("wininet.dll", SetLastError = true)] static extern bool InternetSetOptionW(IntPtr h, int option, IntPtr buf, int len);

  const string Key = @"Software\Microsoft\Windows\CurrentVersion\Internet Settings";
  static object[] saved;
  static readonly string[] Values = { "ProxyEnable", "ProxyServer", "ProxyOverride", "AutoConfigURL" };
  static readonly string[] Blobs = { "DefaultConnectionSettings", "SavedLegacySettings" };

  public static void Save() {
    saved = new object[Values.Length + Blobs.Length];
    using (var k = Registry.CurrentUser.OpenSubKey(Key)) {
      for (int i = 0; i < Values.Length; i++) saved[i] = k == null ? null : k.GetValue(Values[i]);
    }
    using (var k = Registry.CurrentUser.OpenSubKey(Key + @"\Connections")) {
      for (int i = 0; i < Blobs.Length; i++) saved[Values.Length + i] = k == null ? null : k.GetValue(Blobs[i]);
    }
  }

  public static void Restore() {
    if (saved == null) return;
    using (var k = Registry.CurrentUser.CreateSubKey(Key)) {
      for (int i = 0; i < Values.Length; i++) {
        if (saved[i] == null) k.DeleteValue(Values[i], false); else k.SetValue(Values[i], saved[i]);
      }
    }
    using (var k = Registry.CurrentUser.CreateSubKey(Key + @"\Connections")) {
      for (int i = 0; i < Blobs.Length; i++) {
        object v = saved[Values.Length + i];
        if (v == null) k.DeleteValue(Blobs[i], false); else k.SetValue(Blobs[i], v, RegistryValueKind.Binary);
      }
    }
    Refresh();
  }

  static void Refresh() {
    InternetSetOptionW(IntPtr.Zero, 95 /* PROXY_SETTINGS_CHANGED */, IntPtr.Zero, 0);
    InternetSetOptionW(IntPtr.Zero, 39 /* SETTINGS_CHANGED */, IntPtr.Zero, 0);
    InternetSetOptionW(IntPtr.Zero, 37 /* REFRESH */, IntPtr.Zero, 0);
  }

  // flags: PROXY_TYPE_DIRECT 1 | PROXY_TYPE_PROXY 2 | PROXY_TYPE_AUTO_PROXY_URL 4.
  static bool Set(int flags, string server, string bypass, string pac) {
    var strings = new IntPtr[] { Marshal.StringToHGlobalUni(server ?? ""), Marshal.StringToHGlobalUni(bypass ?? ""),
        Marshal.StringToHGlobalUni(pac ?? "") };
    int size = Marshal.SizeOf(typeof(Option));
    IntPtr options = Marshal.AllocHGlobal(size * 4);
    IntPtr list = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OptionList)));
    try {
      Marshal.StructureToPtr(new Option { dwOption = 1, value = (IntPtr)flags }, options, false);
      Marshal.StructureToPtr(new Option { dwOption = 2, value = strings[0] }, options + size, false);
      Marshal.StructureToPtr(new Option { dwOption = 3, value = strings[1] }, options + 2 * size, false);
      Marshal.StructureToPtr(new Option { dwOption = 4, value = strings[2] }, options + 3 * size, false);
      var l = new OptionList { dwSize = Marshal.SizeOf(typeof(OptionList)), pszConnection = IntPtr.Zero,
          dwOptionCount = 4, pOptions = options };
      Marshal.StructureToPtr(l, list, false);
      bool ok = InternetSetOptionW(IntPtr.Zero, 75 /* PER_CONNECTION_OPTION */, list, l.dwSize);
      Refresh();
      return ok;
    } finally {
      Marshal.FreeHGlobal(list); Marshal.FreeHGlobal(options);
      foreach (var s in strings) Marshal.FreeHGlobal(s);
    }
  }

  public static bool SetServer(string server, string bypass) { return Set(1 | 2, server, bypass, null); }
  public static bool SetPac(string url) { return Set(1 | 4, null, null, url); }
}
'@
}
