# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
param([Parameter(Mandatory = $true)][string]$Dir)

$ErrorActionPreference = 'Stop'
$sourceDir = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runDir = (Resolve-Path $Dir).Path
$receiptPath = Join-Path $runDir 'build-receipt.json'
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.schema -cne 'gesher-native-callback-cleanup-build-v1') { throw 'Unexpected cleanup build receipt' }
function Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$commit = (& git -C $sourceDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read checked-out commit' }
$tree = (& git -C $sourceDir rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read checked-out tree' }
if ($receipt.source.commit -cne $commit -or $receipt.source.tree -cne $tree) {
    throw 'Build and native runner source identities differ'
}
if ($receipt.source.files.PSObject.Properties.Name -cnotcontains 'compat/arm64/dll/host_call.h') {
    throw 'Cleanup receipt omits the production HostCall definition'
}
foreach ($item in $receipt.source.files.PSObject.Properties) {
    if ((Sha256 (Join-Path $sourceDir $item.Name)) -cne $item.Value) {
        throw "Source hash mismatch: $($item.Name)"
    }
}
foreach ($item in $receipt.outputs.PSObject.Properties) {
    if ($item.Name -cnotmatch '^[A-Za-z0-9_.-]+$' -or
        (Sha256 (Join-Path $runDir $item.Name)) -cne $item.Value) {
        throw "Build output hash mismatch: $($item.Name)"
    }
}
if ($env:GITHUB_ACTIONS -ceq 'true') {
    foreach ($key in 'GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT',
                     'GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID') {
        if ($receipt.ci.$key -cne [Environment]::GetEnvironmentVariable($key)) {
            throw "Producer/consumer run identity mismatch: $key"
        }
    }
    if ($receipt.source.dirty -or $env:RUNNER_OS -cne 'Windows' -or
        $env:RUNNER_ARCH -cne 'X64' -or
        $env:CLEANUP_EXPECTED_RUNNER -cnotin @('windows-2022','windows-2025') -or
        $env:CLEANUP_BUILD_ARTIFACT_ID -cnotmatch '^[0-9]+$' -or
        $env:CLEANUP_BUILD_ARTIFACT_DIGEST -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Unexpected native gate source, runner or artifact identity'
    }
}

$cases = @(
    @{ Name='supported'; Arg=$null; Exit=0; Marker='native callback cleanup: passed; cases=81 ' },
    @{ Name='foreign-normal'; Arg='--foreign-normal'; Exit=0; Marker='native callback cleanup: passed; cases=1 ' },
    @{ Name='catch-all'; Arg='--catch-all'; Exit=71; Marker='UNSUPPORTED catch-all intercepted forced transfer' },
    @{ Name='noexcept'; Arg='--noexcept'; Exit=72; Marker='UNSUPPORTED noexcept boundary terminated forced transfer' },
    @{ Name='foreign-finally'; Arg='--foreign-finally'; Exit=73; Marker='UNSUPPORTED foreign SEH finally was not executed' },
    @{ Name='raw-rejected'; Arg='--raw-rejected'; Exit=74; Marker='REJECTED raw native jump skipped owned C++ cleanup; not a portable baseline' }
)
$results = @()
$allPassed = $true
$exe = Join-Path $runDir 'native_cleanup_test.exe'
foreach ($case in $cases) {
    $stdout = Join-Path $runDir ($case.Name + '.stdout.txt')
    $stderr = Join-Path $runDir ($case.Name + '.stderr.txt')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $startArgs = @{
        FilePath=$exe; PassThru=$true; NoNewWindow=$true;
        WorkingDirectory=$runDir; RedirectStandardOutput=$stdout; RedirectStandardError=$stderr
    }
    if ($case.Arg) { $startArgs.ArgumentList = @($case.Arg) }
    $process = Start-Process @startArgs
    $finished = $process.WaitForExit(30000)
    if (-not $finished) {
        & taskkill.exe /PID $process.Id /T /F | Out-Null
        $process.WaitForExit(5000) | Out-Null
    } else {
        $process.WaitForExit()
    }
    $watch.Stop()
    $process.Refresh()
    $exitCode = if ($process.HasExited) { $process.ExitCode } else { $null }
    $text = [IO.File]::ReadAllText($stdout)
    $errors = [IO.File]::ReadAllText($stderr)
    $passed = $finished -and $exitCode -eq $case.Exit -and
              $text.Contains($case.Marker) -and -not $text.Contains('FAIL ') -and
              -not $text.Contains('FATAL ')
    if (-not $passed) { $allPassed = $false }
    Write-Host "NATIVE_CLEANUP name=$($case.Name) exit=$exitCode expected=$($case.Exit) timeout=$(-not $finished) passed=$passed"
    Write-Host $text
    if ($errors) { Write-Host $errors }
    $results += [ordered]@{
        name=$case.Name; argument=$case.Arg; exit_code=$exitCode;
        expected_exit_code=$case.Exit; timed_out=(-not $finished);
        seconds=$watch.Elapsed.TotalSeconds; required_marker=$case.Marker;
        passed=$passed; stdout_sha256=(Sha256 $stdout); stderr_sha256=(Sha256 $stderr)
    }
    $process.Dispose()
}
$record = [ordered]@{
    schema='gesher-native-callback-cleanup-run-v1'; source_commit=$commit; source_tree=$tree;
    build_receipt_sha256=(Sha256 $receiptPath);
    executable_sha256=(Sha256 $exe);
    helper_dll_sha256=(Sha256 (Join-Path $runDir 'native_cleanup_helper.dll'));
    producer_artifact_id=$env:CLEANUP_BUILD_ARTIFACT_ID;
    producer_artifact_digest=$env:CLEANUP_BUILD_ARTIFACT_DIGEST;
    run_id=$env:GITHUB_RUN_ID; run_attempt=$env:GITHUB_RUN_ATTEMPT;
    repository=$env:GITHUB_REPOSITORY; expected_runner=$env:CLEANUP_EXPECTED_RUNNER;
    actual_os=[Environment]::OSVersion.VersionString;
    image_os=$env:ImageOS; image_version=$env:ImageVersion;
    process_architecture=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString();
    scope='Native LLVM cleanup qualification only; no production callback transfer';
    results=$results; passed=$allPassed
}
$record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runDir 'run-receipt.json') -Encoding utf8
if (-not $allPassed) { throw 'Native cleanup qualification or a specific boundary characterization failed' }
