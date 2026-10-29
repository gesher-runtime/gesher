$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Native fixture is CI-only' }
$trusted = @{
  'tools/windows4/TsfJobSupervisor.cs'='d1cf9319268df1bccf03177e7f8dde52a7f55510'
  'tools/windows4/TsfJobWorker.cs'='08f29f78f56195825d96de2f2f6632bf91133e1e'
  'tools/windows4/test-tsf-job-supervisor.ps1'='91e6946c4ab05a42ffbf6917166193c3dad07872'
  'tools/native3-locks/artifact/download-verified-artifact.py'='cd5cc415e53102c664edac22b1604df639c8804b'
}
foreach ($entry in $trusted.GetEnumerator()) {
  if ((git hash-object $entry.Key) -cne $entry.Value -or $LASTEXITCODE -ne 0) { throw "Unreviewed helper $($entry.Key)" }
}
if ((git rev-parse HEAD) -cne $env:GITHUB_SHA -or $LASTEXITCODE -ne 0) { throw 'Checkout mismatch' }
$jobsRaw = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read producer jobs' }
$jobs = @(($jobsRaw | ConvertFrom-Json).jobs | Where-Object name -CEQ 'compile')
if ($jobs.Count -ne 1 -or $jobs[0].conclusion -cne 'success' -or "$($jobs[0].run_id)" -cne $env:GITHUB_RUN_ID -or "$($jobs[0].run_attempt)" -cne $env:GITHUB_RUN_ATTEMPT -or $jobs[0].head_sha -cne $env:GITHUB_SHA) { throw 'Producer mismatch' }
$raw = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect artifact inventory' }
$rows = @(($raw | ConvertFrom-Json).artifacts | Where-Object name -CEQ "tcp-priority-$env:GITHUB_SHA-a$env:GITHUB_RUN_ATTEMPT")
if ($rows.Count -ne 1) { throw 'Artifact not unique' }
$a = $rows[0]
if ($a.id -le 0 -or $a.size_in_bytes -le 0 -or $a.expired -or $a.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or "$($a.workflow_run.id)" -cne $env:GITHUB_RUN_ID -or "$($a.workflow_run.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or "$($a.workflow_run.head_repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or $a.workflow_run.head_sha -cne $env:GITHUB_SHA -or [datetime]$a.created_at -lt [datetime]$jobs[0].started_at -or [datetime]$a.created_at -gt [datetime]$jobs[0].completed_at) { throw 'Artifact source/run/attempt provenance mismatch' }
python tools/native3-locks/artifact/download-verified-artifact.py --id "$($a.id)" --digest $a.digest --destination tcp-fixture
if ($LASTEXITCODE -ne 0) { throw 'Raw archive digest or extraction failed' }
$m = Get-Content -LiteralPath tcp-fixture/provenance.json -Raw | ConvertFrom-Json
if ($m.repository -cne $env:GITHUB_REPOSITORY -or "$($m.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or $m.source -cne $env:GITHUB_SHA -or "$($m.run)" -cne $env:GITHUB_RUN_ID -or "$($m.attempt)" -cne $env:GITHUB_RUN_ATTEMPT -or $m.producer_job -cne 'compile' -or $m.scope -cne 'noninline-only') { throw 'Manifest identity mismatch' }
$roles = @{'gesher-exec.exe'='host-bionic-loader'; 'net_tcp_priority_probe.exe'='host-posix-contract'; 'gesher_arm64.dll'='matching-arm64-translator'; 'tcp-priority-x86_64'='ndk-x86_64-contract'; 'tcp-priority-arm64-v8a'='ndk-arm64-contract'}
if (@(Compare-Object @($roles.Keys) @($m.outputs.PSObject.Properties.Name) -CaseSensitive).Count -ne 0 -or @(Compare-Object @(@($roles.Keys) + 'provenance.json') @(Get-ChildItem tcp-fixture -Force | ForEach-Object { if ($_.PSIsContainer) { throw 'Unexpected artifact directory' }; $_.Name }) -CaseSensitive).Count -ne 0) { throw 'Unexpected artifact members or roles' }
$tracked = @(git ls-files)
if ($LASTEXITCODE -ne 0 -or @(Compare-Object $tracked @($m.sources.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw 'Incomplete source inventory' }
foreach ($source in $m.sources.PSObject.Properties) {
  if ((Get-FileHash -LiteralPath $source.Name -Algorithm SHA256).Hash.ToLowerInvariant() -cne $source.Value) { throw "Source mismatch $($source.Name)" }
}
foreach ($name in $roles.Keys) {
  $record = $m.outputs.$name
  if ($record.role -cne $roles[$name] -or (Get-FileHash -LiteralPath "tcp-fixture/$name" -Algorithm SHA256).Hash.ToLowerInvariant() -cne $record.sha256) { throw "Role or member digest mismatch $name" }
}
$publisher = $m.ndk_archive
if ($publisher.url -cne 'https://dl.google.com/android/repository/android-ndk-r27c-linux.zip' -or $publisher.publisher_sha1 -cne '090e8083a715fdb1a3e402d0763c388abb03fb4e' -or $publisher.bytes -ne 663987688 -or $publisher.revision -cne '27.2.12479018' -or $publisher.archive_sha256 -cnotmatch '^[0-9a-f]{64}$' -or $publisher.properties_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $publisher.extraction) { throw 'NDK publisher receipt mismatch' }
$native = @('gesher-exec.exe','net_tcp_priority_probe.exe','gesher_arm64.dll')
if (@(Compare-Object $native @($m.imports.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw 'Incomplete native import role proof' }
$policyPath = 'tools/net-tcp-priority/import-policy.json'
if ((Get-FileHash -LiteralPath $policyPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $m.import_policy_sha256) { throw 'Import policy source mismatch' }
$policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
function AssertImportRole([string] $Role, [string[]] $Names) {
  if ($Role -cnotin $policy.PSObject.Properties.Name -or $Names.Count -eq 0 -or @($Names | ForEach-Object ToLowerInvariant | Select-Object -Unique).Count -ne $Names.Count) { throw 'Invalid native role or empty/duplicate imports' }
  foreach ($dll in $Names) {
    $name = $dll.ToLowerInvariant()
    if ($name -cnotmatch '^[a-z0-9_-]+\.dll$' -or ($name -cnotin $policy.$Role.system -and $name -cnotin @('msvcrt.dll','ucrtbase.dll') -and $name -cnotmatch '^api-ms-win-crt-[a-z0-9-]+-l1-1-0\.dll$')) { throw "Unexpected native role dependency $Role/$dll" }
  }
}
foreach ($role in $native) {
  AssertImportRole $role @('kernel32.dll','api-ms-win-crt-runtime-l1-1-0.dll')
  foreach ($bad in 'unknown.dll','gesher_private.dll','../kernel32.dll','userenv.dll') {
    $rejected = $false
    try { AssertImportRole $role @('kernel32.dll', $bad) } catch { $rejected = $true }
    if (-not $rejected) { throw "Import policy negative control accepted $role/$bad" }
  }
}
foreach ($role in 'gesher_arm64.dll','net_tcp_priority_probe.exe') {
  $rejected = $false
  try { AssertImportRole $role @('WINHTTP.dll') } catch { $rejected = $true }
  if (-not $rejected) { throw "Wrong-role negative control accepted $role" }
}
Write-Host 'PASS TCP priority consumer import policy: unknown/private/path/wrong-role controls'
foreach ($name in $native) { AssertImportRole $name @($m.imports.$name) }
Write-Host ('TCP_PRIORITY_NDK_PUBLISHER ' + ($publisher | ConvertTo-Json -Compress))
$commandSources = @{
  'out'=@('compat/posix/src/winsock.cc','compat/posix/src/socket.cc','compat/posix/src/fd.cc','compat/posix/src/epoll.cc','compat/posix/src/select.cc','compat/bionic/socket_bionic.cc','tests/net-tcp-priority/contract.cc')
  'out-arm64'=@('compat/arm64/dll/translator.cc')
}
foreach ($build in 'out','out-arm64') {
  $c = $m.compilers.$build
  $expectedCount = $commandSources[$build].Count
  if (@(Compare-Object $commandSources[$build] @($c.commands.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw "Wrong compiled source roles $build" }
  if (-not $c.configured -or -not $c.resolved -or $c.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $c.metadata_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $c.version -or @($c.commands.PSObject.Properties).Count -ne $expectedCount) { throw "Missing configured compiler evidence $build" }
  foreach ($command in $c.commands.PSObject.Properties) { if (-not $command.Value.Contains($c.configured) -or -not $command.Value.Contains('-Werror')) { throw "Missing actual source/compiler invocation $($command.Name)" } }
}
foreach ($abi in 'x86_64','arm64-v8a') {
  $c = $m.compilers.ndk.$abi
  if (-not $c.configured -or -not $c.resolved -or $c.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $c.backend_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $c.backend_resolved -or -not $c.version -or '-Werror' -cnotin $c.argv -or 'tests/net-tcp-priority/contract.cc' -cnotin $c.argv) { throw "Missing NDK compiler evidence $abi" }
  $triple = if ($abi -eq 'x86_64') { 'x86_64-linux-android' } else { 'aarch64-linux-android' }
  if (-not $c.configured.StartsWith($publisher.extraction + '/toolchains/llvm/prebuilt/linux-x86_64/bin/', [StringComparison]::Ordinal) -or -not $c.configured.EndsWith($triple + '29-clang++', [StringComparison]::Ordinal) -or "tcp-fixture/tcp-priority-$abi" -cnotin $c.argv) { throw "NDK archive/ABI/compiler binding mismatch $abi" }
  $elf = [IO.File]::ReadAllBytes((Resolve-Path "tcp-fixture/tcp-priority-$abi").Path)
  $machine = if ($abi -eq 'x86_64') { 62 } else { 183 }
  if ($elf.Length -lt 64 -or $elf[0] -ne 127 -or $elf[1] -ne 69 -or $elf[2] -ne 76 -or $elf[3] -ne 70 -or $elf[4] -ne 2 -or $elf[5] -ne 1 -or [BitConverter]::ToUInt16($elf, 18) -ne $machine) { throw "Wrong ELF ABI $abi" }
}
Write-Host "TCP_PRIORITY_RECEIPT source=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT producer=$($jobs[0].id) artifact=$($a.id) digest=$($a.digest) scope=noninline-only"
Write-Host ($m.compilers | ConvertTo-Json -Depth 8 -Compress)
& ./tools/windows4/test-tsf-job-supervisor.ps1
if (-not ('TsfJobSupervisor' -as [type])) { Add-Type -Path tools/windows4/TsfJobSupervisor.cs }
$env:GESHER_INET = '1'
$fixture = (Resolve-Path tcp-fixture).Path
function RunPriority([string] $Context, [string] $Exe, [string] $Arguments) {
  $result = [TsfJobSupervisor]::Run($Exe, $Arguments, 30000, '')
  Write-Host ('TCP_PRIORITY_KERNEL ' + ($result | ConvertTo-Json -Depth 6 -Compress))
  $text = $result.Output + $result.Error
  Write-Host $text
  if (-not $result.Success -or $result.ExitCode -ne 0 -or $text -match '(?m)^FAIL' -or $text -match '(?m)^PASS TCP priority: inline=1') { throw "Actual $Context fixture failed" }
  foreach ($label in @('inline=0 queue/repoll/consume/select/epoll/dup/multi/normal/timeout/EOF', 'failed connection and closed descriptor distinguish errors and preserve SO_ERROR', 'delayed arrival bounded wake and cleanup', 'noninline contract checks passed; inline remains OPEN')) {
    if ($text -notmatch ('(?m)^PASS TCP priority: ' + [regex]::Escape($label) + '\s*$')) { throw "Missing actual assertion $Context/$label" }
  }
  if ($text -notmatch '(?m)^PASS TCP priority: listener datagram and successful connect preserve finite priority-only waits\s*$') { throw "Missing finite endpoint controls $Context" }
  Write-Host "PASS TCP priority component: $Context; actual noninline contract; inline remains OPEN"
}
RunPriority 'host-posix' "$fixture/net_tcp_priority_probe.exe" '--noninline'
foreach ($abi in 'x86_64','arm64-v8a') {
  $elfPath = (Join-Path $fixture "tcp-priority-$abi").Replace('\','/') -replace '^[A-Za-z]:',''
  RunPriority "bionic-$abi" "$fixture/gesher-exec.exe" "--elf `"$elfPath`" tcp-priority --noninline"
}
Write-Host 'PASS TCP priority component: all three real paths; noninline only; inline OPEN'
