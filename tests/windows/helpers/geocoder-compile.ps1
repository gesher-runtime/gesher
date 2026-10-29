# CI producer: compile tracked helpers before any real-runtime child starts.
# The compiler is the owned job ROOT; runtime allowlists remain unchanged.
function Assert-GeocoderOwnedJob($r, [string] $Executable, [string] $Role) {
  Write-Host ('WINRT_JOB_KERNEL_EVIDENCE role=' + $Role + ' ' + ($r | ConvertTo-Json -Depth 6 -Compress))
  if (-not $r.Success -or $r.Reason -cne 'exit' -or $r.ExitCode -ne 0 -or $r.Errors.Count -ne 0) { throw "WinRT $Role did not succeed" }
  if (-not $r.Assigned -or -not $r.MemberBeforeResume -or -not $r.SuspendedSingleThread -or $r.PreResumeActive -ne 1 -or $r.SuspendedThreadCount -ne 1) { throw "WinRT $Role assignment missing" }
  if ($r.Pid -eq 0 -or $r.Birth -le 0 -or $r.RootHandleValue -eq 0 -or $r.JobHandleValue -eq 0 -or -not [string]::Equals($r.Image, $Executable, [StringComparison]::OrdinalIgnoreCase)) { throw "WinRT $Role identity missing" }
  if (-not $r.RootSignaled -or $r.RootWaitResult -ne 0 -or -not $r.RootHandleClosed -or -not $r.ThreadHandleClosed -or -not $r.JobEmpty -or $r.LastJobActive -ne 0 -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.CleanupMilliseconds -ge 5000) { throw "WinRT $Role not reaped" }
  if (-not $r.WritersRetired -or -not $r.OutputRead -or -not $r.ExclusiveSnapshot -or $r.StdoutBytes -gt 1048576 -or $r.StderrBytes -gt 1048576) { throw "WinRT $Role output retirement missing" }
  if ($r.TotalProcesses -ne $r.Processes.Count -or @($r.Processes | Where-Object { -not $_.Member -or -not $_.Signaled -or -not $_.Closed -or $_.WaitResult -ne 0 -or $_.Birth -le 0 -or $_.HandleValue -eq 0 }).Count -ne 0) { throw "WinRT $Role child census missing" }
}
function New-GeocoderProbeAssembly {
  if ($env:GITHUB_ACTIONS -cne 'true') { throw 'WinRT compilation is CI only' }
  . (Join-Path $PSScriptRoot '../tracks/geocoder-operation.ps1')
  if (-not ('TsfJobSupervisor' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot '../../../tools/windows4/TsfJobSupervisor.cs') }
  $framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
  $compiler = [IO.Path]::GetFullPath((Join-Path $framework 'csc.exe'))
  $sources = @('geocoder-probe-exit.cs', 'geocoder-winrt-supplier.cs') | ForEach-Object {
    $path = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../tracks/$_"))
    @{ Path = $path; Sha256 = (Get-GeocoderSourceSha256 $path) }
  }
  $references = @('mscorlib.dll', 'System.dll', 'System.Core.dll') | ForEach-Object {
    $path = [IO.Path]::GetFullPath((Join-Path $framework $_))
    @{ Path = $path; Sha256 = (Get-GeocoderSourceSha256 $path) }
  }
  $directory = Join-Path $env:RUNNER_TEMP ('geocoder-compiled-' + [Guid]::NewGuid().ToString('N'))
  $null = [IO.Directory]::CreateDirectory($directory)
  $script:geocoderCompiledDirectory = $directory
  $dll = Join-Path $directory 'GesherGeocoderProbes.dll'
  $argv = @('/nologo', '/noconfig', '/nostdlib+', '/target:library', '/optimize+', ('/out:"' + $dll + '"'))
  $argv += @($references | ForEach-Object { '/reference:"' + $_.Path + '"' })
  $argv += @($sources | ForEach-Object { '"' + $_.Path + '"' })
  $receipt = @{
    Repository = $env:GITHUB_REPOSITORY; RunId = $env:GITHUB_RUN_ID; RunAttempt = $env:GITHUB_RUN_ATTEMPT; Role = 'geocoder-helper-compiler'
    Producer = 'Direct compiler owned by TsfJobSupervisor'; PowerShell = $PSVersionTable.PSVersion.ToString(); Clr = [Environment]::Version.ToString()
    SourceRevision = $env:GITHUB_SHA; Sources = @($sources); References = @($references)
    Compiler = $compiler; CompilerSha256 = (Get-GeocoderSourceSha256 $compiler)
    CompilerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($compiler).FileVersion
    Arguments = ($argv -join ' '); ArgumentVector = $argv; Output = $dll
    ReferenceContract = 'Explicit .NET Framework 4 references; helpers use no WinRT metadata. Runtime projects Windows.Services.Maps and System.Runtime.WindowsRuntime in Windows PowerShell 5.'
  }
  Write-Host ('WINRT_COMPILER_START ' + ($receipt | ConvertTo-Json -Depth 6 -Compress))
  $compiled = [TsfJobSupervisor]::Run($compiler, $receipt.Arguments, 45000, '')
  Assert-GeocoderOwnedJob $compiled $compiler 'compiler'
  Write-Host $compiled.Output
  if (-not [IO.File]::Exists($dll)) { throw 'Compiler produced no DLL' }
  foreach ($dependency in @($sources) + @($references)) {
    if ((Get-GeocoderSourceSha256 $dependency.Path) -cne $dependency.Sha256) { throw 'Compiler input changed' }
  }
  if ((Get-GeocoderSourceSha256 $compiler) -cne $receipt.CompilerSha256) { throw 'Compiler changed' }
  $receipt.OutputSha256 = Get-GeocoderSourceSha256 $dll
  $json = $receipt | ConvertTo-Json -Depth 6 -Compress
  $manifest = Join-Path $directory 'receipt.json'
  [IO.File]::WriteAllText($manifest, $json)
  $env:GESHER_GEOCODER_RECEIPT = $manifest
  $env:GESHER_GEOCODER_RECEIPT_SHA256 = Get-GeocoderSourceSha256 $manifest
  Write-Host ('WINRT_COMPILER_RECEIPT ' + $json)
}

function Remove-GeocoderProbeAssembly {
  # Delete only the two files in the unique directory created by this producer.
  # No recursive deletion and no process cleanup by PID.
  try {
  if ($script:geocoderCompiledDirectory) {
    $directory = [IO.Path]::GetFullPath($script:geocoderCompiledDirectory)
    $tempPrefix = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
    if (-not $directory.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($directory) -notmatch '^geocoder-compiled-[0-9a-f]{32}$') { throw 'Invalid producer cleanup path' }
    [IO.File]::Delete((Join-Path $directory 'receipt.json'))
    [IO.File]::Delete((Join-Path $directory 'GesherGeocoderProbes.dll'))
    [IO.Directory]::Delete($directory, $false)
    $script:geocoderCompiledDirectory = $null
    Write-Host 'WINRT_COMPILER_CLEANUP files_removed=True directory_removed=True'
  }
  } finally {
  $env:GESHER_GEOCODER_RECEIPT = $null
  $env:GESHER_GEOCODER_RECEIPT_SHA256 = $null
  }
}
