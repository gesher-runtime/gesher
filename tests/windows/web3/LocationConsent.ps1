# Test-only UI driver: consent to this fixture's exact two-permission request.
# The production controller still authenticates, prompts and persists its decision.
if (-not ('Web3LocationConsent' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class Web3LocationConsent {
  delegate bool EnumWindow(IntPtr window, IntPtr data);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback, IntPtr data);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr window, int id);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
  [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  static extern int GetClassName(IntPtr window, StringBuilder name, int size);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageTimeoutW")]
  static extern IntPtr ReadText(IntPtr window, uint message, IntPtr size, StringBuilder text,
      uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW")]
  static extern IntPtr SendCommand(IntPtr window, uint message, IntPtr w, IntPtr l,
      uint flags, uint timeout, out IntPtr result);
  static string Text(IntPtr window) {
    if (window == IntPtr.Zero) return "";
    var text = new StringBuilder(4096);
    IntPtr result;
    return ReadText(window, 0x000D, (IntPtr)text.Capacity, text, 2, 500, out result) != IntPtr.Zero
        ? text.ToString() : "";
  }
  public static bool Respond(int owner, bool allow) {
    return Respond(owner, allow, 0);
  }
  public static bool Respond(int owner, bool allow, long started) {
    if (owner <= 0) return false;
    try {
      using (var process = Process.GetProcessById(owner)) {
        if (process.HasExited || (started != 0 && process.StartTime.Ticks != started)) return false;
        return RespondToOwnedWindow(process, allow);
      }
    } catch (ArgumentException) { return false; }
      catch (InvalidOperationException) { return false; }
      catch (System.ComponentModel.Win32Exception) { return false; }
  }
  static bool RespondToOwnedWindow(Process owner, bool allow) {
    var matches = new List<IntPtr>();
    EnumWindows((window, data) => {
      uint process;
      GetWindowThreadProcessId(window, out process);
      var kind = new StringBuilder(64);
      GetClassName(window, kind, kind.Capacity);
      if (process != (uint)owner.Id || kind.ToString() != "#32770" || !IsWindowVisible(window)) return true;
      if (Text(GetDlgItem(window, 1101)) != "com.gesher.web3app") return true;
      var details = Text(GetDlgItem(window, 1102));
      var permissions = System.Text.RegularExpressions.Regex.Matches(details, @"android\.permission\.[A-Z_]+");
      var expected = new HashSet<string> { "android.permission.ACCESS_COARSE_LOCATION", "android.permission.ACCESS_FINE_LOCATION" };
      if (permissions.Count != 2) return true;
      foreach (System.Text.RegularExpressions.Match permission in permissions)
        if (!expected.Remove(permission.Value)) return true;
      if (expected.Count == 0 && GetDlgItem(window, 6) != IntPtr.Zero
          && GetDlgItem(window, 2) != IntPtr.Zero) matches.Add(window);
      return true;
    }, IntPtr.Zero);
    if (matches.Count != 1) return false;
    var dialog = matches[0];
    int id = allow ? 6 : 2; // IDYES / IDCANCEL
    var button = GetDlgItem(dialog, id);
    uint processId;
    GetWindowThreadProcessId(button, out processId);
    if (processId != (uint)owner.Id || owner.HasExited || !IsWindowEnabled(button)) return false;
    IntPtr ignored;
    // BM_CLICK needs an active dialog. Dispatch the equivalent BN_CLICKED
    // to this exact test dialog without moving desktop focus.
    if (SendCommand(dialog, 0x0111, (IntPtr)id, button, 2, 500, out ignored) == IntPtr.Zero) return false; // WM_COMMAND / BN_CLICKED
    // A modal dialog can finish unwinding after the notification returns.
    var dismissal = Stopwatch.StartNew();
    while (IsWindowVisible(dialog) && dismissal.ElapsedMilliseconds < 500)
      System.Threading.Thread.Sleep(10);
    return !IsWindowVisible(dialog);
  }
}
'@
}

# The standalone VM can stand in as a supervisor while its canonical-UID child
# runs the APK. Bound the search to the actual launcher and its one VM restart.
function Get-Web3StandaloneConsentOwner([Diagnostics.Process] $Launcher, [string] $Runtime) {
  if ($Launcher.HasExited) { return }
  $direct = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($Launcher.Id)" |
    Where-Object { $_.ExecutablePath -eq $Runtime -and $_.CreationDate -ge $Launcher.StartTime })
  if ($direct.Count -ne 1) { return }
  $first = $direct[0]
  $restarts = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($first.ProcessId)" |
    Where-Object { $_.ExecutablePath -eq $Runtime -and $_.CommandLine -ceq $first.CommandLine -and
      $_.CreationDate -ge $first.CreationDate })
  if ($restarts.Count -gt 1 -or $Launcher.HasExited) { return }
  $active = if ($restarts.Count -eq 1) { $restarts[0] } else { $first }
  try {
    $process = Get-Process -Id $active.ProcessId -ErrorAction SilentlyContinue
    if (-not $process) { return }
    try {
      # CIM timestamps have microsecond precision; the open Process gives the
      # exact creation time for the final window ownership check.
      if ($process.HasExited -or [Math]::Abs(($process.StartTime - $active.CreationDate).Ticks) -gt 10) { return }
      [pscustomobject]@{ Id = $process.Id; Started = $process.StartTime.Ticks }
    } finally { $process.Dispose() }
  } catch [ArgumentException] {} catch [InvalidOperationException] {} catch [System.ComponentModel.Win32Exception] {}
}
