# Source-only, distinct plain handshake fixture. Never sends WriteRead or buffer ACKs.
param(
  [string]$BinderdPath,
  [string]$Device,
  [switch]$Hold
)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only; this source is not locally qualified' }

function ConnectPeer {
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', "gesher-binder-$Device",
    [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
  $pipe.Connect(10000)
  return $pipe
}
function Request($pipe, [byte[]]$body) {
  $writer = [System.IO.BinaryWriter]::new($pipe, [System.Text.Encoding]::UTF8, $true)
  try { $writer.Write([uint32]$body.Length); $writer.Write($body); $writer.Flush() }
  finally { $writer.Dispose() }
  $reader = [System.IO.BinaryReader]::new($pipe, [System.Text.Encoding]::UTF8, $true)
  try {
    $length = $reader.ReadUInt32()
    if ($length -gt 64) { throw "oversized handshake reply $length" }
    $bytes = $reader.ReadBytes($length)
    if ($bytes.Length -ne $length) { throw 'short handshake reply' }
    return ,$bytes
  } finally { $reader.Dispose() }
}
function ProcHello([uint32]$claimedPid, [uint32]$uid) {
  $stream = [System.IO.MemoryStream]::new()
  $writer = [System.IO.BinaryWriter]::new($stream)
  try {
    foreach ($value in @([uint32]1, [uint32]0x47424452, [uint32]2, $claimedPid, $uid)) {
      $writer.Write([uint32]$value)
    }
    return ,$stream.ToArray()
  } finally { $writer.Dispose(); $stream.Dispose() }
}
function ThreadHello([uint32]$procId, [uint64]$key) {
  $stream = [System.IO.MemoryStream]::new()
  $writer = [System.IO.BinaryWriter]::new($stream)
  try {
    foreach ($value in @([uint32]2, [uint32]0x47424452, [uint32]2, $procId)) {
      $writer.Write([uint32]$value)
    }
    $writer.Write($key)
    # TID is not part of this fixture's identity assertion.
    $writer.Write([uint32]1)
    return ,$stream.ToArray()
  } finally { $writer.Dispose(); $stream.Dispose() }
}
function OpenSelf {
  $pipe = ConnectPeer
  try {
    $reply = Request $pipe (ProcHello ([uint32]$PID) 10000)
    if ($reply.Length -ne 16 -or [BitConverter]::ToInt32($reply, 0) -ne 0) {
      throw 'honest process hello rejected'
    }
    return @{ Pipe = $pipe; Id = [BitConverter]::ToUInt32($reply, 4);
      Key = [BitConverter]::ToUInt64($reply, 8) }
  } catch { $pipe.Dispose(); throw }
}
function ThreadStatus($identity) {
  $pipe = ConnectPeer
  try {
    $reply = Request $pipe (ThreadHello $identity.Id $identity.Key)
    if ($reply.Length -ne 4) { throw 'invalid thread status frame' }
    return [BitConverter]::ToInt32($reply, 0)
  } finally { $pipe.Dispose() }
}
function CheckThread($identity, [int]$expected) {
  if ((ThreadStatus $identity) -ne $expected) { throw "thread identity status differed from $expected" }
}

if ($Hold) {
  $self = OpenSelf
  try {
    CheckThread $self 0
    [Console]::Out.WriteLine((@{ Pid = $PID; Id = $self.Id; Key = $self.Key } | ConvertTo-Json -Compress))
    [Console]::Out.Flush()
    $null = [Console]::In.ReadLine()
  } finally { $self.Pipe.Dispose() }
  exit 0
}
if (-not $BinderdPath -or -not [IO.File]::Exists($BinderdPath)) { throw 'exact rebuilt binderd path required' }
$Device = 'peer-identity-' + [guid]::NewGuid().ToString('N')
$broker = $null; $victim = $null; $self = $null
try {
  $start = [Diagnostics.ProcessStartInfo]::new($BinderdPath)
  $start.UseShellExecute = $false; $start.CreateNoWindow = $true
  $start.ArgumentList.Add($Device)
  $broker = [Diagnostics.Process]::Start($start)
  # Child is a real, different, live pipe client; credentials are only shared
  # with this controlled attacker to exercise key possession without ownership.
  $child = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
  $child.UseShellExecute = $false; $child.CreateNoWindow = $true
  $child.RedirectStandardOutput = $true; $child.RedirectStandardInput = $true
  foreach ($arg in @('-NoProfile', '-File', $PSCommandPath, '-Device', $Device, '-Hold')) {
    $child.ArgumentList.Add($arg)
  }
  $victim = [Diagnostics.Process]::Start($child)
  $line = $victim.StandardOutput.ReadLineAsync()
  if (-not $line.Wait(15000)) { throw 'victim registration timeout' }
  $identity = $line.Result | ConvertFrom-Json
  if (-not $identity -or $victim.HasExited -or $identity.Pid -ne $victim.Id) {
    throw 'missing live controlled victim'
  }
  foreach ($claimedUid in @(10000, 1000)) {
    $pipe = ConnectPeer
    try {
      $reply = Request $pipe (ProcHello ([uint32]$victim.Id) ([uint32]$claimedUid))
      if ($reply.Length -ne 4 -or [BitConverter]::ToInt32($reply, 0) -ne -1) {
        throw "forged live victim PID/UID $claimedUid received authority"
      }
    } finally { $pipe.Dispose() }
  }
  CheckThread $identity -3 # valid victim proc/key over attacker transport
  $self = OpenSelf
  CheckThread $self 0
  $self.Pipe.Dispose(); $self.Pipe = $null
  # A released Proc/key must not be revived even by its original process.
  # Release is asynchronous: retry only the negative until broker observes EOF.
  $released = $false
  for ($attempt = 0; $attempt -lt 100; $attempt++) {
    $status = ThreadStatus $self
    if ($status -eq -3) { $released = $true; break }
    if ($status -ne 0) { throw "unexpected release status $status" }
    Start-Sleep -Milliseconds 20
  }
  if (-not $released) { throw 'released proc remained attachable' }
  $victim.StandardInput.WriteLine('exit'); $victim.StandardInput.Flush()
  if (-not $victim.WaitForExit(10000) -or $victim.ExitCode -ne 0) { throw 'victim failed' }
  Write-Host 'PASS peer process hello: forged live PID/UID rejected; foreign valid key rejected; honest proc/thread and release controls'
  Write-Host 'SOURCE LIMIT: logical UID registry/bootstrap and registered-package controls remain absent; no UID authentication claim'
} finally {
  if ($self -and $self.Pipe) { $self.Pipe.Dispose() }
  foreach ($owned in @($victim, $broker)) {
    if ($owned) { if (-not $owned.HasExited) { $owned.Kill(); $owned.WaitForExit() }; $owned.Dispose() }
  }
}
