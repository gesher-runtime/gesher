# Read-only evidence after a failed native fixture. The caller preserves the
# native exit code; collecting diagnostics cannot change its test verdict.
[CmdletBinding(DefaultParameterSetName = 'Collect')]
param(
  [Parameter(Mandatory, ParameterSetName = 'Collect')][string] $LogPath,
  [Parameter(Mandatory, ParameterSetName = 'Worker')][switch] $Worker,
  [Parameter(Mandatory, ParameterSetName = 'Worker')][string] $ProfileSid,
  [Parameter(Mandatory, ParameterSetName = 'Worker')][string] $FixtureStartUtc,
  [Parameter(Mandatory, ParameterSetName = 'Worker')][string] $FixtureEndUtc
)

$ErrorActionPreference = 'Stop'
$diagnosticEntryUtc = [DateTimeOffset]::UtcNow
$utcFormat = "yyyy-MM-dd'T'HH:mm:ss.ffffffzzz"
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Write-ProfileObservation([string] $Kind, [hashtable] $Details) {
  $record = @{ observation = $Kind; details = $Details } | ConvertTo-Json -Depth 5 -Compress
  [Console]::Out.WriteLine('Protected profile diagnostics: ' + $record)
}

function Get-ProfileErrorText($Record) {
  $text = [string]$Record.Exception.Message
  if ($text.Length -gt 1024) { return $text.Substring(0, 1024) + ' [truncated]' }
  return $text
}

function Assert-OwnedServiceSid([string] $Value) {
  if ($Value -cnotmatch '\AS-1-5-80(?:-(?:0|[1-9][0-9]{0,9})){5}\z') {
    throw 'Owned-profile marker is not a canonical virtual-service SID.'
  }
  foreach ($component in $Value.Split('-')[4..8]) {
    $number = [uint32]0
    if (-not [uint32]::TryParse($component, [Globalization.NumberStyles]::None, $invariant, [ref]$number)) {
      throw 'Owned-profile SID component is outside uint32.'
    }
  }
  $identity = [Security.Principal.SecurityIdentifier]::new($Value)
  if ($identity.Value -cne $Value) { throw 'Owned-profile SID failed canonical validation.' }
}

function Read-FixtureUtc([string] $Value) {
  $instant = [DateTimeOffset]::ParseExact($Value, $utcFormat, $invariant, [Globalization.DateTimeStyles]::None)
  if ($instant.Offset -ne [TimeSpan]::Zero -or $instant.ToString($utcFormat, $invariant) -cne $Value) {
    throw 'Fixture timestamp must use the canonical UTC format.'
  }
  return $instant
}

try {
  if (-not $Worker) {
    # The same file is open for append by CMD. Read at most 1 MiB without
    # denying that writer, and never interpret fixture output as PowerShell.
    $stream = [IO.File]::Open($LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
      $reader = [IO.BinaryReader]::new($stream)
      try { $bytes = $reader.ReadBytes(1048577) } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    if ($bytes.Length -gt 1048576) { throw 'Native log exceeds the 1 MiB diagnostic acquisition cap.' }
    $lines = [Text.Encoding]::UTF8.GetString($bytes) -split '\r?\n'
    $sidPrefix = 'Protected launch: owned-profile-sid='
    $startPrefix = 'Protected launch: fixture-start-utc='
    $sidLines = @($lines | Where-Object { $_.StartsWith($sidPrefix, [StringComparison]::Ordinal) })
    $startLines = @($lines | Where-Object { $_.StartsWith($startPrefix, [StringComparison]::Ordinal) })
    if ($sidLines.Count -ne 1 -or $startLines.Count -ne 1) {
      throw "Expected one owned SID and one fixture start marker; observed $($sidLines.Count) and $($startLines.Count)."
    }
    $ownedSid = $sidLines[0].Substring($sidPrefix.Length)
    Assert-OwnedServiceSid $ownedSid
    $start = Read-FixtureUtc ($startLines[0].Substring($startPrefix.Length))
    $end = $diagnosticEntryUtc
    $span = ($end - $start).TotalSeconds
    if ($span -lt 0 -or $span -gt 180) { throw 'Fixture interval is outside the existing 180-second guard.' }
    Write-ProfileObservation 'scope' @{
      sid = $ownedSid; start_utc = $start.ToString($utcFormat, $invariant)
      end_utc = $end.ToString($utcFormat, $invariant); worker_timeout_seconds = 10
      process_reader_cleanup_bound_seconds = 25; inspected_event_cap_per_log = 128
      emitted_event_cap_per_log = 8; emitted_xml_character_cap = 8192
    }
    . "$PSScriptRoot/../../tests/windows/helpers/GtestProcess.ps1"
    $result = Invoke-GtestProcess -Executable (Join-Path $PSHOME 'pwsh.exe') -Arguments @(
      '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $PSCommandPath, '-Worker',
      '-ProfileSid', $ownedSid, '-FixtureStartUtc', $start.ToString($utcFormat, $invariant),
      '-FixtureEndUtc', $end.ToString($utcFormat, $invariant)
    ) -WorkingDirectory (Get-Location).Path -TimeoutSec 10
    [Console]::Out.Write($result.Stdout)
    [Console]::Out.Write($result.Stderr)
    Write-ProfileObservation 'worker-lifetime' @{
      exit = $result.Exit; timed_out = $result.TimedOut; process_complete = $result.ProcessComplete
      readers_complete = $result.ReadersComplete; reader_tasks_complete = $result.ReaderTasksComplete
      pipes_closed = $result.PipesClosed; disposed = $result.Disposed; problem = $result.Problem
    }
    if ($result.TimedOut) { exit 124 }
    if (-not $result.ProcessComplete -or -not $result.ReadersComplete -or
        -not $result.ReaderTasksComplete -or -not $result.PipesClosed -or
        -not $result.Disposed -or $result.Problem -or $result.Exit -ne 0) { exit 1 }
    exit 0
  }

  # This branch runs only in the exact owned process under the finite parent
  # helper. A stalled CIM/event provider cannot block the native CI verdict.
  Assert-OwnedServiceSid $ProfileSid
  $start = Read-FixtureUtc $FixtureStartUtc
  $end = Read-FixtureUtc $FixtureEndUtc
  if (($end - $start).TotalSeconds -lt 0 -or ($end - $start).TotalSeconds -gt 180) {
    throw 'Invalid worker fixture interval.'
  }
  $observationErrors = 0
  try {
    $profiles = @(CimCmdlets\Get-CimInstance -Namespace root/cimv2 -ClassName Win32_UserProfile `
      -Filter "SID='$ProfileSid'" -Property SID, Loaded, RefCount, Special, Status, LocalPath -OperationTimeoutSec 3)
    if ($profiles.Count -gt 1 -or @($profiles | Where-Object { $_.SID -cne $ProfileSid }).Count) {
      throw 'CIM returned a non-unique or mismatched profile identity.'
    }
    if ($profiles.Count -eq 0) { Write-ProfileObservation 'profile-not-found' @{ sid = $ProfileSid } }
    else {
      $profile = $profiles[0]
      Write-ProfileObservation 'profile' @{
        SID = $profile.SID; Loaded = $profile.Loaded; RefCount = $profile.RefCount
        Special = $profile.Special; Status = $profile.Status; LocalPath = $profile.LocalPath
      }
    }
  } catch {
    $observationErrors++
    Write-ProfileObservation 'profile-query-error' @{ message = (Get-ProfileErrorText $_) }
  }

  $sidPattern = '(?<![A-Za-z0-9-])' + [regex]::Escape($ProfileSid) + '(?![A-Za-z0-9-])'
  foreach ($channel in @('Application', 'Microsoft-Windows-User Profile Service/Operational')) {
    $events = @()
    try {
      $events = @(Microsoft.PowerShell.Diagnostics\Get-WinEvent -FilterHashtable @{
        LogName = $channel; ProviderName = 'Microsoft-Windows-User Profiles Service'
        StartTime = $start.UtcDateTime; EndTime = $end.UtcDateTime
      } -MaxEvents 129)
      $inspected = 0
      $matched = 0
      $emitted = 0
      foreach ($event in $events) {
        if ($inspected -ge 128) { break }
        $inspected++
        $xml = $event.ToXml()
        if ($xml -cnotmatch $sidPattern) { continue }
        $matched++
        if ($emitted -ge 8) { continue }
        $truncated = $xml.Length -gt 8192
        if ($truncated) { $xml = $xml.Substring(0, 8192) }
        Write-ProfileObservation 'profile-event' @{
          channel = $channel; id = $event.Id; record_id = $event.RecordId
          time_utc = $event.TimeCreated.ToUniversalTime().ToString('O', $invariant)
          xml = $xml; xml_truncated = $truncated
        }
        $emitted++
      }
      Write-ProfileObservation 'event-query' @{
        channel = $channel; inspected = $inspected; sid_matches = $matched; emitted = $emitted
        inspection_truncated = $events.Count -gt 128; emission_truncated = $matched -gt 8
      }
    } catch {
      if ($_.FullyQualifiedErrorId.StartsWith('NoMatchingEventsFound', [StringComparison]::Ordinal)) {
        Write-ProfileObservation 'event-query-empty' @{ channel = $channel }
      } else {
        $observationErrors++
        Write-ProfileObservation 'event-query-error' @{ channel = $channel; message = (Get-ProfileErrorText $_) }
      }
    } finally {
      foreach ($event in $events) { $event.Dispose() }
    }
  }
  if ($observationErrors) { exit 1 }
} catch {
  Write-ProfileObservation 'unavailable' @{ message = (Get-ProfileErrorText $_) }
  exit 1
}
