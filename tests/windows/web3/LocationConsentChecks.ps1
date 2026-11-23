# Synthetic offscreen native dialogs exercise only the test UI driver's matching.
# They do not claim to exercise Android permission grants or WebView geolocation.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/LocationConsent.ps1"
$fixtureRoot = Join-Path $env:TEMP ("gesher-consent-checks-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$source = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public class ConsentFixture {
  delegate IntPtr DialogProc(IntPtr w, uint m, IntPtr p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr DialogBoxIndirectParamW(IntPtr i, IntPtr t, IntPtr p, DialogProc d, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string c, string t, uint s, int x, int y, int w, int h, IntPtr p, IntPtr id, IntPtr i, IntPtr l);
  [DllImport("user32.dll")] static extern bool EndDialog(IntPtr w, IntPtr r);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr w);
  [DllImport("user32.dll")] static extern IntPtr SetTimer(IntPtr w, IntPtr id, uint ms, IntPtr callback);
  [DllImport("user32.dll")] static extern bool KillTimer(IntPtr w, IntPtr id);
  [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr w);
  [DllImport("user32.dll")] static extern IntPtr GetActiveWindow();
  static string package, details;
  static bool inactive;
  static IntPtr Proc(IntPtr w, uint m, IntPtr p, IntPtr l) {
    if (m == 0x110) {
      CreateWindowExW(0, "EDIT", package, 0x50000800, 0,0,80,20,w,(IntPtr)1101,IntPtr.Zero,IntPtr.Zero);
      CreateWindowExW(0, "EDIT", details, 0x50000804, 0,20,80,20,w,(IntPtr)1102,IntPtr.Zero,IntPtr.Zero);
      CreateWindowExW(0, "BUTTON", "Deny", 0x5001000B, 0,40,40,20,w,(IntPtr)2,IntPtr.Zero,IntPtr.Zero);
      CreateWindowExW(0, "BUTTON", "Allow", 0x5001000B, 40,40,40,20,w,(IntPtr)6,IntPtr.Zero,IntPtr.Zero);
      SetTimer(w,(IntPtr)1,30,IntPtr.Zero);
      return (IntPtr)1;
    }
    if (m == 0x113 && IsWindowVisible(w)) {
      KillTimer(w,(IntPtr)1);
      if (inactive) {
        var other = CreateWindowExW(0, "STATIC", "", 0x10CF0000, -10000,-10000,80,30,
            IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);
        SetActiveWindow(other);
        if (other == IntPtr.Zero || GetActiveWindow() != other) Environment.Exit(91);
      }
      Console.WriteLine("READY"); Console.Out.Flush(); return (IntPtr)1;
    }
    if (m == 0x111 && (p.ToInt64() == 2 || p.ToInt64() == 6)) { EndDialog(w, p); return (IntPtr)1; }
    return IntPtr.Zero;
  }
  static int Dialog() {
    IntPtr t=Marshal.AllocHGlobal(24);
    for(int n=0;n<24;n++) Marshal.WriteByte(t,n,0);
    Marshal.WriteInt32(t,0,unchecked((int)0x80C80080));
    Marshal.WriteInt16(t,10,-10000); Marshal.WriteInt16(t,12,-10000);
    Marshal.WriteInt16(t,14,100); Marshal.WriteInt16(t,16,80);
    DialogProc proc=Proc;
    int r=DialogBoxIndirectParamW(IntPtr.Zero,t,IntPtr.Zero,proc,IntPtr.Zero).ToInt32();
    GC.KeepAlive(proc); Marshal.FreeHGlobal(t); return r;
  }
  public static int Main(string[] args) {
    package=args[0]; details=args[1];
    inactive = args.Length > 2 && args[2] == "inactive";
    if(args.Length > 2 && args[2] == "ambiguous") { new Thread(()=>Dialog()).Start(); }
    return Dialog();
  }
}
'@
$code = Join-Path $fixtureRoot 'ConsentFixture.cs'
$exe = Join-Path $fixtureRoot 'ConsentFixture.exe'
[IO.File]::WriteAllText($code,$source)
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" /nologo "/out:$exe" $code
if($LASTEXITCODE) { throw 'fixture compile failed' }
$location = 'android.permission.ACCESS_COARSE_LOCATION android.permission.ACCESS_FINE_LOCATION'
foreach($case in @(
  @{Name='allow';Package='com.gesher.web3app';Details=$location;Allow=$true;Expected=6},
  @{Name='deny';Package='com.gesher.web3app';Details=$location;Allow=$false;Expected=2},
  @{Name='inactive-deny';Package='com.gesher.web3app';Details=$location;Allow=$false;Expected=2;Inactive=$true},
  @{Name='other-package';Package='com.example.other';Details=$location;Expected=0},
  @{Name='extra-permission';Package='com.gesher.web3app';Details="$location android.permission.CAMERA";Expected=0},
  @{Name='missing-permission';Package='com.gesher.web3app';Details='android.permission.ACCESS_FINE_LOCATION';Expected=0},
  @{Name='duplicate-permission';Package='com.gesher.web3app';Details='android.permission.ACCESS_FINE_LOCATION android.permission.ACCESS_FINE_LOCATION';Expected=0},
  @{Name='ambiguous';Package='com.gesher.web3app';Details=$location;Expected=0;Ambiguous=$true}
)) {
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName=$exe; $psi.Arguments='"'+$case.Package+'" "'+$case.Details+'"'+$(if($case.Ambiguous){' ambiguous'}elseif($case.Inactive){' inactive'}else{''})
  $psi.UseShellExecute=$false; $psi.RedirectStandardOutput=$true; $psi.CreateNoWindow=$true
  $process=[Diagnostics.Process]::Start($psi)
  try {
    $line=$process.StandardOutput.ReadLineAsync()
    if(-not $line.Wait(5000) -or $line.Result -ne 'READY') { throw 'dialog never initialized' }
    if($case.Ambiguous) {
      $line=$process.StandardOutput.ReadLineAsync()
      if(-not $line.Wait(5000) -or $line.Result -ne 'READY') { throw 'second dialog never initialized' }
    }
    if([Web3LocationConsent]::Respond($PID,$true)) { throw 'wrong owner accepted' }
    if([Web3LocationConsent]::Respond($process.Id,$true,$process.StartTime.AddSeconds(-1).Ticks)) {
      throw 'wrong process generation accepted'
    }
    $acted=[Web3LocationConsent]::Respond($process.Id,[bool]$case.Allow,$process.StartTime.Ticks)
    if($case.Expected) {
      if(-not $acted -or -not $process.WaitForExit(5000) -or $process.ExitCode -ne $case.Expected) { throw "$($case.Name) failed" }
    } elseif($acted) { throw "$($case.Name) accepted" }
    Write-Host "PASS synthetic native dialog: $($case.Name)"
  } finally { if(-not $process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose() }
}
