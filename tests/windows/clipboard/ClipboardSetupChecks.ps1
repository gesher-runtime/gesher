param([switch] $Windows)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/NativeForeground.ps1"
$checks = 0
function Assert-ClipboardSetup([bool] $Okay, [string] $Reason) {
  $script:checks++
  if (-not $Okay) { throw $Reason }
}

# These isolated command-boundary controls run on Linux too. The real Windows
# gate below does not replace either clipboard command or a Win32 API.
& {
  $script:sets = 0; $script:gets = 0; $script:stored = 'previous native text'
  $script:failSets = 0; $script:failGets = 0; $script:delayRead = 0
  $script:throwSet = $false; $script:wrongText = $false
  function Set-Clipboard {
    [CmdletBinding()] param([string] $Value)
    $script:sets++
    if ($script:throwSet) { throw 'explicit native publication error' }
    if ($script:sets -gt $script:failSets) { $script:stored = $Value }
  }
  function Get-Clipboard {
    [CmdletBinding()] param([switch] $Raw)
    $script:gets++
    if ($script:delayRead) { Start-Sleep -Milliseconds $script:delayRead }
    if ($script:wrongText -or $script:gets -le $script:failGets) { return 'different native text' }
    return $script:stored
  }
  Set-GesherClipboardText -Text 'immediate'
  Assert-ClipboardSetup ($script:sets -eq 1 -and $script:gets -eq 1) 'Immediate exact readback should need one publication'

  $script:sets = 0; $script:gets = 0; $script:stored = 'old'; $script:failSets = 2
  # The original two-call prerequisite fails on the same silent command result.
  Set-Clipboard -Value 'challenge'
  Assert-ClipboardSetup ((Get-Clipboard -Raw) -cne 'challenge') 'Original immediate prerequisite did not expose denied publication'
  $script:sets = 0; $script:gets = 0
  Set-GesherClipboardText -Text 'challenge'
  Assert-ClipboardSetup ($script:sets -eq 3 -and $script:gets -eq 3 -and $script:stored -ceq 'challenge') 'Retry must establish the exact real command readback'

  $script:sets = 0; $script:gets = 0; $script:failSets = 0; $script:failGets = 2
  Set-GesherClipboardText -Text 'read retry'
  Assert-ClipboardSetup ($script:gets -eq 3 -and $script:stored -ceq 'read retry') 'Transient unreadable clipboard must not signal early success'

  $script:failGets = 0; $script:wrongText = $true
  $timed = [Diagnostics.Stopwatch]::StartNew(); $rejected = $false
  try { Set-GesherClipboardText -Text 'never observed' -TimeoutMilliseconds 120 }
  catch { $rejected = $_.Exception.Message -match 'exact readback was not established' }
  Assert-ClipboardSetup ($rejected -and $timed.ElapsedMilliseconds -ge 120 -and $timed.ElapsedMilliseconds -lt 2000) 'Persistent wrong content must reach the fixed deadline and fail'

  $script:wrongText = $false; $script:delayRead = 100; $script:sets = 0
  $rejected = $false
  try { Set-GesherClipboardText -Text 'late match' -TimeoutMilliseconds 40 }
  catch { $rejected = $_.Exception.Message -match 'exact readback was not established' }
  Assert-ClipboardSetup ($rejected -and $script:sets -eq 1) 'A matching readback after the deadline must not succeed or reset the deadline'

  $script:delayRead = 0; $script:throwSet = $true; $script:sets = 0; $script:gets = 0
  $rejected = $false
  try { Set-GesherClipboardText -Text 'error' }
  catch { $rejected = $_.Exception.Message -eq 'explicit native publication error' }
  Assert-ClipboardSetup ($rejected -and $script:sets -eq 1 -and $script:gets -eq 0) 'Explicit command failures must propagate without retry or readback'
}

if ($Windows) {
  if (-not $IsWindows) { throw 'The real contention gate requires Windows' }
  # The holding thread performs only actual Open/CloseClipboard, never data
  # replacement. Entry is acknowledged only after OpenClipboard succeeds.
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;
public sealed class ClipboardSetupReader : IDisposable {
  [DllImport("user32.dll", SetLastError = true)] static extern bool OpenClipboard(IntPtr window);
  [DllImport("user32.dll", SetLastError = true)] static extern bool CloseClipboard();
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr CreateWindowExW(uint extendedStyle, string name, string title,
      uint style, int x, int y, int width, int height, IntPtr parent,
      IntPtr menu, IntPtr instance, IntPtr parameter);
  [DllImport("user32.dll", SetLastError = true)] static extern bool DestroyWindow(IntPtr window);
  [DllImport("user32.dll")] static extern IntPtr GetOpenClipboardWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
  [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint format);
  [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr value);
  [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr value);
  readonly ManualResetEventSlim entered = new ManualResetEventSlim();
  readonly ManualResetEventSlim release = new ManualResetEventSlim();
  readonly Thread thread;
  Thread releaser;
  Exception failure;
  IntPtr window;
  uint openThread, closeThread, destroyThread;
  int closeError, destroyError;
  bool closed, destroyed;
  public volatile bool Held;
  public bool OwnsClipboard { get { return Held && window != IntPtr.Zero && GetOpenClipboardWindow() == window; } }
  public ClipboardSetupReader() {
    thread = new Thread(() => {
      try {
        openThread = GetCurrentThreadId();
        // PowerShell's real Set/Get use OpenClipboard(NULL). Give the holder
        // a distinct actual window identity, owned and destroyed on this thread.
        window = CreateWindowExW(0, "STATIC", "clipboard setup reader", 0,
            0, 0, 0, 0, new IntPtr(-3) /* HWND_MESSAGE */, IntPtr.Zero,
            IntPtr.Zero, IntPtr.Zero);
        if (window == IntPtr.Zero)
          throw new Win32Exception(Marshal.GetLastWin32Error(), "Native reader window creation failed");
        uint process;
        if (GetWindowThreadProcessId(window, out process) != openThread || process != GetCurrentProcessId())
          throw new InvalidOperationException("Native reader window has a different thread/process owner");
        var timer = System.Diagnostics.Stopwatch.StartNew();
        while (!OpenClipboard(window)) {
          int error = Marshal.GetLastWin32Error();
          if (timer.ElapsedMilliseconds >= 2000)
            throw new Win32Exception(error, "Actual native reader could not open clipboard");
          Thread.Sleep(1);
        }
        Held = true;
        entered.Set();
        try {
          if (!release.Wait(5000)) throw new InvalidOperationException("Native reader release was not requested");
        } catch (Exception ex) {
          failure = ex;
        } finally {
          closeThread = GetCurrentThreadId();
          closed = CloseClipboard();
          closeError = closed ? 0 : Marshal.GetLastWin32Error();
          Held = false;
          if (!closed) throw new Win32Exception(closeError, "Actual native reader failed to close clipboard");
        }
      } catch (Exception ex) {
        failure = failure == null ? ex : new AggregateException(failure, ex);
        entered.Set();
      }
      finally {
        if (window != IntPtr.Zero) {
          destroyThread = GetCurrentThreadId();
          destroyed = DestroyWindow(window);
          destroyError = destroyed ? 0 : Marshal.GetLastWin32Error();
          if (destroyed) window = IntPtr.Zero;
          else {
            var error = new Win32Exception(destroyError, "Native reader window destruction failed");
            failure = failure == null ? error : new AggregateException(failure, error);
          }
        }
      }
    });
    thread.IsBackground = true;
    thread.Start();
    if (!entered.Wait(3000) || !Held) {
      release.Set();
      if (!thread.Join(3000)) throw new InvalidOperationException("Failed native reader did not retire");
      entered.Dispose(); release.Dispose();
      throw failure ?? new InvalidOperationException("Native reader did not acknowledge actual clipboard ownership");
    }
  }
  public void ReleaseAfter(int milliseconds) {
    releaser = new Thread(() => { Thread.Sleep(milliseconds); release.Set(); });
    releaser.IsBackground = true;
    releaser.Start();
  }
  public static string ReadText() {
    var timer = System.Diagnostics.Stopwatch.StartNew();
    while (!OpenClipboard(IntPtr.Zero)) {
      if (timer.ElapsedMilliseconds >= 1000) throw new InvalidOperationException("Native readback could not open clipboard");
      Thread.Sleep(1);
    }
    try {
      IntPtr handle = GetClipboardData(13 /* CF_UNICODETEXT */);
      if (handle == IntPtr.Zero) throw new InvalidOperationException("Native readback has no Unicode text");
      IntPtr text = GlobalLock(handle);
      if (text == IntPtr.Zero) throw new InvalidOperationException("Native readback could not lock text");
      try { return Marshal.PtrToStringUni(text); }
      finally { GlobalUnlock(handle); }
    } finally {
      if (!CloseClipboard()) throw new InvalidOperationException("Native readback failed to close clipboard");
    }
  }
  public void Dispose() {
    release.Set();
    if (!thread.Join(3000) || (releaser != null && !releaser.Join(3000)))
      throw new InvalidOperationException("Native reader threads did not retire");
    entered.Dispose(); release.Dispose();
    Console.WriteLine("CLIPBOARD_SETUP_READER closed={0} destroyed={1} open_thread={2} close_thread={3} destroy_thread={4} close_error={5} destroy_error={6}",
        closed, destroyed, openThread, closeThread, destroyThread, closeError, destroyError);
    if (failure != null) throw failure;
    if (!closed || !destroyed || openThread != closeThread || openThread != destroyThread)
      throw new InvalidOperationException("Native reader did not close and destroy its window on the owning thread");
  }
}
'@
  function Complete-ClipboardSetupReader($Reader, [Exception] $PrimaryFailure) {
    try { $Reader.Dispose() }
    catch {
      if ($PrimaryFailure) {
        throw [AggregateException]::new('Clipboard assertion and owned reader cleanup both failed',
            [Exception[]]@($PrimaryFailure, $_.Exception))
      }
      throw
    }
  }
  $token = 'clipboard-setup-' + [Guid]::NewGuid().ToString('N')
  Set-GesherClipboardText -Text ($token + '-prior')
  $reader = [ClipboardSetupReader]::new()
  $primaryFailure = $null
  try {
    Assert-ClipboardSetup $reader.Held 'Actual native reader must own clipboard before the negative control'
    Assert-ClipboardSetup $reader.OwnsClipboard 'Real open clipboard must identify the distinct holder HWND'
    Set-Clipboard -Value $token
    Assert-ClipboardSetup ((Get-Clipboard -Raw) -cne $token) 'Original immediate Set/Get must fail while the real reader holds clipboard'
    Assert-ClipboardSetup $reader.Held 'Failed clipboard commands must not release the other thread reader'
    Assert-ClipboardSetup $reader.OwnsClipboard 'Denied native commands must preserve the exact open clipboard HWND'
    $reader.ReleaseAfter(150)
    Set-GesherClipboardText -Text $token
    Assert-ClipboardSetup (-not $reader.Held) 'Publication must wait for the actual reader release'
    Assert-ClipboardSetup ([ClipboardSetupReader]::ReadText() -ceq $token) 'Independent native readback must contain the exact challenge after reader release'
  } catch { $primaryFailure = $_.Exception; throw }
  finally { Complete-ClipboardSetupReader $reader $primaryFailure }

  $reader = [ClipboardSetupReader]::new()
  $primaryFailure = $null
  try {
    Assert-ClipboardSetup $reader.OwnsClipboard 'Persistent contention starts with the exact real holder HWND'
    $rejected = $false; $timed = [Diagnostics.Stopwatch]::StartNew()
    try { Set-GesherClipboardText -Text ($token + '-denied') }
    catch { $rejected = $_.Exception.Message -match 'exact readback was not established' }
    Assert-ClipboardSetup ($rejected -and $timed.ElapsedMilliseconds -ge 1000 -and $timed.ElapsedMilliseconds -lt 4000) 'Persistent actual native contention must fail within the bounded setup interval'
    Assert-ClipboardSetup $reader.Held 'Timeout must not steal or release the real reader ownership'
    Assert-ClipboardSetup $reader.OwnsClipboard 'Setup timeout must preserve the exact open clipboard HWND'
  } catch { $primaryFailure = $_.Exception; throw }
  finally { Complete-ClipboardSetupReader $reader $primaryFailure }
  Assert-ClipboardSetup ([ClipboardSetupReader]::ReadText() -ceq $token) 'Failed publication must leave the previous real clipboard text intact'
}
Write-Host "PASS clipboard setup: $checks exact-readback/deadline/error checks; native_windows=$($Windows.IsPresent)"
