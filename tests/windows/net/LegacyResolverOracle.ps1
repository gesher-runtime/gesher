param([Parameter(Mandatory)][string] $Destination)
$ErrorActionPreference = 'Stop'
# CI-only independent WinSDK calls and explicit Win64 structure decoding.
# This process never loads Gesher, the shim or the NDK test libraries.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class Net4LegacyOracle {
  [DllImport("ws2_32.dll")] static extern int WSAStartup(ushort version, IntPtr data);
  [DllImport("ws2_32.dll")] static extern int WSACleanup();
  [DllImport("ws2_32.dll")] static extern int WSAGetLastError();
  [DllImport("ws2_32.dll", CharSet=CharSet.Ansi)] static extern IntPtr gethostbyname(string name);
  [DllImport("ws2_32.dll")] static extern IntPtr gethostbyaddr(byte[] address, int length, int family);
  [DllImport("ws2_32.dll", CharSet=CharSet.Ansi)] static extern IntPtr getservbyname(string name, string protocol);
  static string Text(IntPtr pointer) {
    if (pointer == IntPtr.Zero) throw new Exception("null WinSDK string");
    return Marshal.PtrToStringAnsi(pointer);
  }
  static string Names(IntPtr table) {
    if (table == IntPtr.Zero) return "";
    var values = new List<string>();
    for (int i=0; i<128; ++i) {
      IntPtr value = Marshal.ReadIntPtr(table, i*8);
      if (value == IntPtr.Zero) { values.Sort(StringComparer.Ordinal); return String.Join(",", values); }
      values.Add(Text(value));
    }
    throw new Exception("unterminated alias table");
  }
  static string Host(string key, IntPtr host) {
    if (host == IntPtr.Zero) throw new Win32Exception(WSAGetLastError(), "WinSDK " + key);
    int family = Marshal.ReadInt16(host,16), length = Marshal.ReadInt16(host,18);
    if (!((family==2 && length==4) || (family==23 && length==16))) throw new Exception("unexpected WinSDK host ABI");
    IntPtr table = Marshal.ReadIntPtr(host,24);
    var addresses = new List<string>();
    for (int i=0; i<128; ++i) {
      IntPtr value = Marshal.ReadIntPtr(table,i*8);
      if (value == IntPtr.Zero) break;
      byte[] bytes = new byte[length]; Marshal.Copy(value,bytes,0,length);
      addresses.Add(Convert.ToHexString(bytes).ToLowerInvariant());
    }
    if (addresses.Count==0) throw new Exception("empty WinSDK address table");
    addresses.Sort(StringComparer.Ordinal);
    return "H|"+key+"|"+(family==23 ? 10 : 2)+"|"+length+"|"+Text(Marshal.ReadIntPtr(host))+"|"
      +Names(Marshal.ReadIntPtr(host,8))+"|"+String.Join(",",addresses);
  }
  static string Service(string name, string protocol) {
    IntPtr service = getservbyname(name,protocol);
    if (service == IntPtr.Zero) throw new Win32Exception(WSAGetLastError(), "WinSDK service " + name);
    ushort port = unchecked((ushort)Marshal.ReadInt16(service,24));
    return "S|"+name+"-"+protocol+"|"+Text(Marshal.ReadIntPtr(service))+"|"+port+"|"
      +Text(Marshal.ReadIntPtr(service,16))+"|"+Names(Marshal.ReadIntPtr(service,8));
  }
  public static string[] Collect() {
    if (IntPtr.Size!=8) throw new Exception("Win64 oracle required");
    IntPtr startup = Marshal.AllocHGlobal(512);
    try { if (WSAStartup(0x0202,startup)!=0) throw new Exception("WSAStartup failed"); }
    finally { Marshal.FreeHGlobal(startup); }
    try {
      var rows = new List<string>();
      rows.Add(Host("numeric",gethostbyname("127.0.0.1")));
      rows.Add(Host("localhost",gethostbyname("localhost")));
      rows.Add(Host("reverse4",gethostbyaddr(new byte[]{127,0,0,1},4,2)));
      byte[] ipv6 = new byte[16]; ipv6[15]=1;
      rows.Add(Host("reverse6",gethostbyaddr(ipv6,16,23)));
      rows.Add(Service("http","tcp")); rows.Add(Service("https","tcp"));
      rows.Add(Service("domain","udp")); rows.Add(Service("domain","tcp"));
      return rows.ToArray();
    } finally { WSACleanup(); }
  }
}
'@
$rows = [Net4LegacyOracle]::Collect()
if ($rows.Count -ne 8 -or @($rows | Select-Object -Unique).Count -ne 8) { throw 'incomplete or duplicate WinSDK oracle' }
[IO.File]::WriteAllLines($Destination, $rows, [Text.UTF8Encoding]::new($false))
foreach ($row in $rows) { Write-Host "DATA net4 WinSDK oracle: $row" }
Write-Host 'WinSDK legacy resolver oracle: eight real host/service rows'
