$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Native fixture is CI-only' }
$trusted = @{
  'tools/windows4/TsfJobSupervisor.cs'='d1cf9319268df1bccf03177e7f8dde52a7f55510'
  'tools/windows4/TsfJobWorker.cs'='08f29f78f56195825d96de2f2f6632bf91133e1e'
  'tools/windows4/test-tsf-job-supervisor.ps1'='91e6946c4ab05a42ffbf6917166193c3dad07872'
  'tools/net-tcp-priority/qualified/TsfJobSupervisor.cs'='625be4d5906bb48dd02d6103382fd2e85ac88738'
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
if ($m.owned_connect_scope -cne 'AF_INET-base-chain1-e70f1aa0-ab8b-11cf-8ca3-00805f48a192-TCP') { throw 'Owned connection provider scope mismatch' }
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
if (@(Compare-Object $native @($m.private_connect_observer.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw 'Missing exact private observer export roles' }
foreach ($name in $native) {
  $proof = $m.private_connect_observer.$name
  if ($proof.observer_exported -cne $false -or $proof.exports -lt 0 -or $proof.exports_sha256 -cnotmatch '^[0-9a-f]{64}$') { throw "Unproved private connect observer role $name" }
}
Write-Host ('TCP_PRIORITY_NDK_PUBLISHER ' + ($publisher | ConvertTo-Json -Compress))
$commandSources = @{
  'out'=@('compat/posix/src/winsock.cc','compat/posix/src/socket.cc','compat/posix/src/fd.cc','compat/posix/src/system.cc','compat/posix/src/epoll.cc','compat/posix/src/select.cc','compat/bionic/socket_bionic.cc','tests/net-tcp-priority/contract.cc','tests/net-tcp-priority/native_udp_control.cc')
  'out-arm64'=@('compat/arm64/dll/translator.cc')
}
foreach ($build in 'out','out-arm64') {
  $c = $m.compilers.$build
  $expectedCount = $commandSources[$build].Count
  if (@(Compare-Object $commandSources[$build] @($c.commands.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw "Wrong compiled source roles $build" }
  if (-not $c.configured -or -not $c.resolved -or $c.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $c.metadata_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $c.version -or @($c.commands.PSObject.Properties).Count -ne $expectedCount) { throw "Missing configured compiler evidence $build" }
  foreach ($command in $c.commands.PSObject.Properties) { if (-not $command.Value.Contains($c.configured) -or -not $command.Value.Contains('-Werror')) { throw "Missing actual source/compiler invocation $($command.Name)" } }
}
$s = $m.supplier
if ($s.role -cne 'static-detached-retirement-experiment-v1' -or $s.producer_job -cne 'compile' -or
    $s.context.GITHUB_SHA -cne $env:GITHUB_SHA -or $s.context.GITHUB_RUN_ID -cne $env:GITHUB_RUN_ID -or
    $s.context.GITHUB_RUN_ATTEMPT -cne $env:GITHUB_RUN_ATTEMPT -or $s.context.GITHUB_REPOSITORY -cne $env:GITHUB_REPOSITORY -or
    $s.context.GITHUB_REPOSITORY_ID -cne $env:GITHUB_REPOSITORY_ID -or $s.upstream -cne 'a3d93999ef0521681d45e445c39964a7af0f593f' -or
    $s.patch_sha256 -cne (Get-FileHash -LiteralPath tools/net-tcp-priority/supplier-retirement.patch -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'Private supplier source/context mismatch' }
if ($s.compiler_archive.sha256 -cne '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4' -or $s.compiler_archive.bytes -le 0 -or
    $s.compiler_archive.url -cne 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz' -or
    $s.private_compiler -cnotmatch '/tcp-private-supplier/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64$') { throw 'Private supplier compiler archive mismatch' }
$sourcePins = @{
  'mingw-w64-libraries/winpthreads/src/thread.c'='c2fcaa54269f0e773069aeeec8212cac71f417483bf65d5e75f100311df4080b'
  'mingw-w64-libraries/winpthreads/src/thread.h'='3c105e10217cabae5a34d1e6e83352fb4eb8a9c2cf048de8053351eb6edf6a25'
  'mingw-w64-libraries/winpthreads/include/pthread.h'='bf3fd920306acb4144873d1099490cf332700c268a8ede0e484c7c0e721c7995'
}
foreach ($pin in $sourcePins.GetEnumerator()) { if ($s.source_before.($pin.Key) -cne $pin.Value) { throw 'Supplier upstream input mismatch' } }
if (@(Compare-Object @($s.source_before.PSObject.Properties.Name) @($s.source_after.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw 'Supplier source inventory changed' }
$changes = @($s.source_before.PSObject.Properties | Where-Object { $_.Value -cne $s.source_after.($_.Name) } | ForEach-Object Name)
if ($changes.Count -ne 1 -or $changes[0] -cne 'mingw-w64-libraries/winpthreads/src/thread.c') { throw 'Supplier patch changed unexpected sources' }
if (@(Compare-Object @($s.public_headers_before.PSObject.Properties.Name) @($s.public_headers_after.PSObject.Properties.Name) -CaseSensitive).Count -ne 0 -or
    $s.public_headers_before.'pthread.h' -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid supplier header inventory' }
foreach ($header in $s.public_headers_before.PSObject.Properties) {
  if ($header.Value -cnotmatch '^[0-9a-f]{64}$' -or $header.Value -cne $s.public_headers_after.($header.Name)) { throw 'Supplier public header changed' }
}
foreach ($build in 'out','out-arm64') {
  if ($m.compilers.$build.configured -cne ($s.private_compiler + '/bin/x86_64-w64-mingw32-clang++') -or
      $m.compilers.$build.sha256 -cne $s.cxx_driver_sha256) { throw 'Native compiler not bound to private supplier' }
}
if (@(Compare-Object $native @($s.native_links.PSObject.Properties.Name) -CaseSensitive).Count -ne 0) { throw 'Incomplete supplier native link roles' }
foreach ($name in $native) {
  $link = $s.native_links.$name
  if ($link.trace_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $link.command.Contains('-Wl,--verbose') -or -not $link.command.Contains('-static') -or
      -not $link.command.Contains($s.private_compiler + '/bin/x86_64-w64-mingw32-clang++')) { throw 'Missing actual private-supplier native link command/trace' }
  $inputs = @($link.inputs.PSObject.Properties)
  foreach ($inspectedArchive in $link.inspected_archives.PSObject.Properties) {
    if ($inspectedArchive.Name -cnotin @('lib/libpthread.a','lib/libwinpthread.a') -or $inspectedArchive.Value -cnotmatch '^[0-9a-f]{64}$' -or
        $inspectedArchive.Value -cne $s.libraries_after.($inspectedArchive.Name)) { throw 'Wrong inspected supplier archive' }
  }
  if ($link.supplier_role -ceq 'not-linked') {
    if ($name -cne 'gesher_arm64.dll' -or $inputs.Count -ne 0 -or @($link.loaded_members).Count -ne 0) { throw 'Invalid no-supplier native role' }
  } elseif ($link.supplier_role -ceq 'static' -and $inputs.Count -gt 0) {
    if (@($link.loaded_members).Count -eq 0) { throw 'Static supplier loaded-member proof missing' }
    foreach ($supplierInput in $inputs) {
      if ($supplierInput.Name -cnotin @('lib/libpthread.a','lib/libwinpthread.a') -or $supplierInput.Value -cnotmatch '^[0-9a-f]{64}$' -or
          $supplierInput.Value -cne $s.libraries_after.($supplierInput.Name) -or
          $supplierInput.Value -cne $link.inspected_archives.($supplierInput.Name)) { throw 'Wrong static supplier link input' }
      $archiveName = $supplierInput.Name.Substring(4)
      if (@($link.loaded_members | Where-Object { $_.member.StartsWith($archiveName + '(', [StringComparison]::Ordinal) }).Count -eq 0) { throw 'Static archive has no actual loaded member' }
    }
    foreach ($member in $link.loaded_members) {
      if ($member.member -cnotmatch '^(lib(?:win)?pthread\.a)\([a-zA-Z0-9_.-]+\)$' -or -not $member.symbol -or
          ('lib/' + $Matches[1]) -cnotin @($inputs.Name)) { throw 'Static supplier member/archive binding mismatch' }
    }
  } else { throw 'Unproved native supplier link role' }
}
Write-Host ('TCP_PRIORITY_SUPPLIER_RECEIPT ' + ($s.native_links | ConvertTo-Json -Depth 6 -Compress))
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
# Exact c228 controller bytes, wrapped only in a fixed namespace to coexist
# with the unchanged original product controller. No diagnostic fault mode.
if (-not ('TcpPriorityDiagnostics.TsfJobSupervisor' -as [type])) {
  $qualifiedController = [IO.File]::ReadAllText((Join-Path $pwd 'tools/net-tcp-priority/qualified/TsfJobSupervisor.cs'))
  Add-Type -TypeDefinition ('namespace TcpPriorityDiagnostics {' + [Environment]::NewLine + $qualifiedController + [Environment]::NewLine + '}')
}
Write-Host ('TCP_PRIORITY_DIAGNOSTIC_CONTROLLER blob=625be4d5906bb48dd02d6103382fd2e85ac88738 sha256=' + (Get-FileHash -LiteralPath tools/net-tcp-priority/qualified/TsfJobSupervisor.cs -Algorithm SHA256).Hash.ToLowerInvariant() + ' namespace=TcpPriorityDiagnostics original_product_controller=unchanged')
function RequireDiagnosticRetirement($Result) {
  if (-not $Result.Success -or $Result.ActiveProcessLimit -ne 1 -or $Result.TotalProcesses -ne 1 -or
      $Result.PreResumeActive -ne 1 -or $Result.Processes.Count -ne 1 -or $Result.Processes[0].Role -cne 'root' -or
      -not $Result.WritersRetired -or -not $Result.ExclusiveSnapshot -or -not $Result.RootHandleClosed -or
      -not $Result.ThreadHandleClosed -or $Result.OwnedHandles -ne $Result.ClosedHandles) { throw 'Isolated diagnostic job/identity/retirement unproved' }
}
$env:GESHER_INET = '1'
$fixture = (Resolve-Path tcp-fixture).Path
# The supplier observation is a SEPARATE fresh process using the authenticated
# same binary/static supplier. It cannot warm the original product census.
$supplier = [TcpPriorityDiagnostics.TsfJobSupervisor]::Run("$fixture/net_tcp_priority_probe.exe", '--supplier-retirement-control', 30000, '')
Write-Host ('TCP_PRIORITY_SUPPLIER_KERNEL ' + ($supplier | ConvertTo-Json -Depth 6 -Compress))
$supplierText = $supplier.Output + $supplier.Error
Write-Host $supplierText
RequireDiagnosticRetirement $supplier
if (-not $supplier.Success -or $supplier.ExitCode -ne 0 -or $supplierText -match '(?m)^FAIL') { throw 'Isolated supplier retirement observation failed' }
foreach ($detached in 0..1) {
  foreach ($cycle in 0..3) {
    if ($supplierText -notmatch "(?m)^TCP_PRIORITY_SUPPLIER_RETIRE detached=$detached cycle=$cycle before=[0-9]+ after=[0-9]+ delta=0 native_exit=1 fixture_closed=2 no_tcp=1\r?$") { throw "Missing exact supplier retirement control $detached/$cycle" }
  }
}
if ($supplierText -notmatch '(?m)^PASS TCP supplier control: exact joined and detached retirement; product census unchanged\s*$') { throw 'Missing exact supplier retirement control' }
$cancellation = [TcpPriorityDiagnostics.TsfJobSupervisor]::Run("$fixture/net_tcp_priority_probe.exe", '--owned-cancellation-control', 30000, '')
Write-Host ('TCP_PRIORITY_CANCEL_KERNEL ' + ($cancellation | ConvertTo-Json -Depth 6 -Compress))
$cancelText = $cancellation.Output + $cancellation.Error
Write-Host $cancelText
RequireDiagnosticRetirement $cancellation
if (-not $cancellation.Success -or $cancellation.ExitCode -ne 0 -or $cancelText -match '(?m)^FAIL') { throw 'Isolated cancellation observation failed' }
if ($cancellation.Pid -eq $supplier.Pid -and $cancellation.Birth -eq $supplier.Birth) { throw 'Cancellation reused supplier process instance' }
if ($cancelText -notmatch '(?m)^TCP_PRIORITY_CANCEL_OBSERVATION canceled=1 completion=111 native_retired=1 operation=0 description=0 handles=[0-9]+/[0-9]+ threads=[0-9]+/[0-9]+\r?$' -or
    $cancelText -notmatch '(?m)^PASS TCP cancellation control: caller references released and original attempt completed; product census unchanged\s*$') { throw 'Missing actual isolated cancellation ownership control' }
function RunPriority([string] $Context, [string] $Exe, [string] $Arguments) {
  $result = [TsfJobSupervisor]::Run($Exe, $Arguments, 30000, '')
  if ($Context -eq 'host-posix' -and $result.Pid -eq $supplier.Pid -and $result.Birth -eq $supplier.Birth) { throw 'Product census reused the supplier process instance' }
  if ($Context -eq 'host-posix' -and $result.Pid -eq $cancellation.Pid -and $result.Birth -eq $cancellation.Birth) { throw 'Product census reused the cancellation process instance' }
  Write-Host ('TCP_PRIORITY_KERNEL ' + ($result | ConvertTo-Json -Depth 6 -Compress))
  $text = $result.Output + $result.Error
  Write-Host $text
  if (-not $result.Success -or $result.ExitCode -ne 0 -or $text -match '(?m)^FAIL' -or $text -match '(?m)^PASS TCP priority: inline=1') { throw "Actual $Context fixture failed" }
  foreach ($label in @('inline=0 queue/repoll/consume/select/epoll/dup/multi/normal/timeout/EOF', 'failed connection and closed descriptor distinguish errors and preserve SO_ERROR', 'delayed arrival bounded wake and cleanup', 'noninline contract checks passed; inline remains OPEN')) {
    if ($text -notmatch ('(?m)^PASS TCP priority: ' + [regex]::Escape($label) + '\s*$')) { throw "Missing actual assertion $Context/$label" }
  }
  if ($text -notmatch '(?m)^PASS TCP priority: listener datagram and successful connect preserve finite priority-only waits\s*$') { throw "Missing finite endpoint controls $Context" }
  if ($text -notmatch '(?m)^PASS TCP priority: shared aliases preserve dup2 targets last-close EOF concurrent urgent poll and UDP payload\s*$') { throw "Missing alias lifetime controls $Context" }
  if ($text -notmatch '(?m)^TCP_PRIORITY_ALIAS_DRAIN stream_recv=0 stream_error=0 before_last_close_recv=-1 before_last_close_error=11\r?$') { throw "Missing actual urgent stream drain and live last-alias control $Context" }
  foreach ($phase in 'old-target','replacement-source','urgent-drained') {
    if ($text -notmatch "(?m)^TCP_PRIORITY_ALIAS_EOF phase=$phase poll=1 revents=[0-9]+ recv=0 error=0\r?$") { throw "Missing strict actual last-alias EOF $Context/$phase" }
  }
  if ($text -notmatch '(?m)^PASS TCP priority: actual connect finalization window consumes refusal once\s*$') { throw "Missing actual finalization window $Context" }
  if ($text -notmatch '(?m)^PASS TCP priority: requester exit retains completion and last-close drains before fd generation reuse\s*$') { throw "Missing requester/last-close/generation controls $Context" }
  if ($text -notmatch '(?m)^PASS TCP priority: independent established reset remains poll-visible and reaches the caller receive\s*$') { throw "Missing independent established-error control $Context" }
  if ($text -notmatch '(?m)^PASS TCP priority: bind and listen transitions invalidate old refusal classification before new transport\s*$') { throw "Missing bind/listen lifecycle controls $Context" }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^TCP_PRIORITY_CONNECT_WINDOW backend=host-private native_ready=1 lock_busy=[01] getter_before_release=1 first=111 second=0\r?$') { throw 'HOST did not complete its real getter while the requester remained paused' }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^PASS TCP priority: real owned mode flips cancellation send deadline and native owner retirement\s*$') { throw 'Missing HOST actual owned wait controls' }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^PASS TCP priority: real poll snapshots reject closed reused and dup2 identities while aliases and other entries remain ready\s*$') { throw 'Missing HOST actual poll snapshot identity controls' }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^PASS TCP priority: real all-invalid native poll errors preserve closed identity simultaneous readiness and genuine failures\s*$') { throw 'Missing HOST actual native poll error controls' }
  if ($Context -eq 'host-posix') {
    foreach ($mode in 0..2) {
      if ($text -notmatch "(?m)^TCP_PRIORITY_NATIVE_POLL_CLOSE mode=$mode native_rc=-?[0-9]+ native_error=[0-9]+ rc=[12] victim=32 other=[01] elapsed_ms=[0-9]+\r?$") { throw "Missing HOST real native poll close window $mode" }
    }
    if ($text -notmatch '(?m)^TCP_PRIORITY_NATIVE_POLL_ERROR rc=-1 errno=22 retired_description=0\r?$') { throw 'Missing HOST genuine native error control' }
    if ($text -notmatch '(?m)^TCP_PRIORITY_NATIVE_POLL_UNOWNED rc=-1 errno=88 retired_description=0\r?$') { throw 'Missing HOST unowned closed native error control' }
  }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^PASS TCP priority: real legacy leases reject stale native calls after CRT reuse while global operations and getters progress\s*$') { throw 'Missing HOST actual legacy lifetime controls' }
  if ($Context -eq 'host-posix' -and $text -notmatch '(?m)^PASS TCP priority: positive native UDP error remains visible outside owned TCP refusal scope\s*$') { throw 'Missing HOST positive actual UDP error control' }
  if ($Context -eq 'host-posix') {
    foreach ($mode in 0..3) {
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNED_WAIT mode=$mode .*native_retired=1\r?$") { throw "Missing HOST actual mode/cancel/deadline context $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNER_CENSUS mode=$mode handles=([0-9]+)/\1 threads=([0-9]+)/\2\r?$") { throw "Missing HOST repeated physical owner census $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_LEGACY_LEASE mode=$mode fd_reused=1 old_call_rc=-1 old_call_errno=9 close_rc=[0-9]+ global_progress=1 replacement_error=0\r?$") { throw "Missing HOST exact legacy lease context $mode" }
    }
  }
  Write-Host "PASS TCP priority component: $Context; actual noninline contract; inline remains OPEN"
}
RunPriority 'host-posix' "$fixture/net_tcp_priority_probe.exe" '--noninline'
foreach ($abi in 'x86_64','arm64-v8a') {
  $elfPath = (Join-Path $fixture "tcp-priority-$abi").Replace('\','/') -replace '^[A-Za-z]:',''
  RunPriority "bionic-$abi" "$fixture/gesher-exec.exe" "--elf `"$elfPath`" tcp-priority --noninline"
}
Write-Host 'PASS TCP priority component: all three real paths; noninline only; inline OPEN'
