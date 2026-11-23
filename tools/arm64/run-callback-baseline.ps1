# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
param([Parameter(Mandatory = $true)][string]$Dir)

$ErrorActionPreference = 'Stop'
$sourceDir = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runDir = (Resolve-Path $Dir).Path
$receiptPath = Join-Path $runDir 'baseline-build-receipt.json'
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
function Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function NoDrive([string]$Path) { return ($Path -replace '\\','/') -replace '^[A-Za-z]:','' }
$commit = (& git -C $sourceDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read checked-out commit' }
$tree = (& git -C $sourceDir rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read checked-out tree' }
if ($receipt.schema -cne 'gesher-callback-pr120-baseline-build-v1' -or
    $receipt.source.commit -cne $commit -or $receipt.source.tree -cne $tree -or
    $receipt.historical.repository -cne 'gesher-runtime/gesher' -or
    $receipt.historical.repository_id -ne 1426153186 -or
    $receipt.historical.run -ne 44162241853 -or $receipt.historical.attempt -ne 1 -or
    $receipt.historical.head -cne '9c226d06c2f4f176d29a58ac51803be3df49fd6f' -or
    $receipt.historical.merge -cne '097fc3e5d936147162cf0379150292fd1e26d274' -or
    $receipt.historical.tree -cne 'fd04f3b0e111f8e9e403a57b6e547a193c883c71' -or
    $receipt.historical.artifacts.'windows-x64'.id -ne 14372697124 -or
    $receipt.historical.artifacts.'windows-x64'.sha256 -cne '5e1b3c3e1af5054d79bd6ebedce82060c29dc35c25fcc5e9d818cde1e7427818' -or
    $receipt.historical.artifacts.arm64.id -ne 14367839302 -or
    $receipt.historical.artifacts.arm64.sha256 -cne 'ec81eff345532cf0f729da6a08b73a5a676c7164509c9d4933b0c5bb7b9a15d5' -or
    $receipt.fixture.source_sha256 -cne '0ed72ca245058757984f8ee72cec975a87949a4a88717a8c8c04c23115fade33' -or
    $receipt.fixture.ndk_archive_sha256 -cne '59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc') {
    throw 'Unexpected historical runtime or current fixture source identity'
}
foreach ($item in $receipt.source.files.PSObject.Properties) {
    if ((Sha256 (Join-Path $sourceDir $item.Name)) -cne $item.Value) {
        throw "Source hash mismatch: $($item.Name)"
    }
}
foreach ($item in $receipt.outputs.PSObject.Properties) {
    if ($item.Name -cnotmatch '^[A-Za-z0-9_.-]+$' -or
        (Sha256 (Join-Path $runDir $item.Name)) -cne $item.Value) {
        throw "Historical bundle hash mismatch: $($item.Name)"
    }
}
if ($env:GITHUB_ACTIONS -ceq 'true') {
    foreach ($key in 'GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT',
                     'GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID') {
        if ($receipt.ci.$key -cne [Environment]::GetEnvironmentVariable($key)) {
            throw "Fixture producer/consumer identity mismatch: $key"
        }
    }
    if ($receipt.source.dirty -or $env:RUNNER_OS -cne 'Windows' -or
        $env:RUNNER_ARCH -cne 'X64' -or
        $env:CALLBACK_BASELINE_EXPECTED_RUNNER -cnotin @('windows-2022','windows-2025') -or
        $env:CALLBACK_BASELINE_ARTIFACT_ID -cnotmatch '^[0-9]+$' -or
        $env:CALLBACK_BASELINE_ARTIFACT_DIGEST -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Unexpected historical baseline consumer or artifact identity'
    }
}

$exe = Join-Path $runDir 'arm64_elf_test.exe'
$cases = @(@{ Name='trivial-preflight'; Guest='trivial_arm64.so'; Selector=$null; Count=0 })
foreach ($selector in @(1,2,3,4,0)) {
    $cases += @{ Name="callback-case-$selector"; Guest='callback_longjmp_arm64.so';
                 Selector=$selector; Count=$(if ($selector -eq 0) {80} else {1}) }
}
$results = @()
$preflightPassed = $false
$collectionPassed = $true
$callbackSemanticsPassed = $true
$previousSelector = [Environment]::GetEnvironmentVariable('GESHER_CALLBACK_JUMP_CASE')
try {
    foreach ($case in $cases) {
        if ($case.Count -ne 0 -and -not $preflightPassed) { break }
        [Environment]::SetEnvironmentVariable('GESHER_CALLBACK_JUMP_CASE', [string]$case.Selector)
        $stdout = Join-Path $runDir ($case.Name + '.stdout.txt')
        $stderr = Join-Path $runDir ($case.Name + '.stderr.txt')
        $guest = NoDrive (Join-Path $runDir $case.Guest)
        $search = NoDrive $runDir
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $startArgs = @{
            FilePath=$exe; ArgumentList=@(('"' + $guest + '"'), ('"' + $search + '"'));
            WorkingDirectory=$runDir; PassThru=$true; NoNewWindow=$true;
            RedirectStandardOutput=$stdout; RedirectStandardError=$stderr
        }
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
        $missingExport = ($text + $errors) -match 'is not implemented \(stubbed\)|unresolved symbol|unresolved import'
        $loaderPassed = $finished -and $exitCode -eq 0 -and -not $missingExport -and
                        $text.Contains('arm64_elf_test all passed')
        $entered = $case.Count -ne 0 -and
                   $text.Contains("callback longjmp contracts: begin (case $($case.Selector))")
        $guestPassed = $case.Count -ne 0 -and
                       $text.Contains("callback longjmp contracts: all passed (0 failures; $($case.Count) cases)")
        $semanticPass = $loaderPassed -and $guestPassed
        if ($case.Count -eq 0) {
            $preflightPassed = $loaderPassed
            $classification = if ($preflightPassed) {'preflight-passed'} else {'inconclusive-setup-or-loader-failure'}
            if (-not $preflightPassed) { $collectionPassed = $false; $callbackSemanticsPassed = $false }
        } else {
            if ($missingExport) {
                $classification = 'inconclusive-missing-runtime-export'
                $collectionPassed = $false
            } elseif (-not $entered -or -not $process.HasExited) {
                $classification = 'inconclusive-fixture-entry-or-process-termination'
                $collectionPassed = $false
            } elseif ($semanticPass) {
                $classification = 'callback-contract-passed'
            } elseif ($text.Contains('FAIL  callback longjmp:') -or
                      $text.Contains('callback longjmp contracts: FAILED')) {
                $classification = 'callback-contract-assertion-failed'
            } elseif (-not $finished) {
                $classification = 'timeout-after-fixture-entry'
            } elseif ($guestPassed) {
                $classification = 'loader-failure-after-guest-success'
            } else {
                $classification = 'runtime-failure-after-fixture-entry'
            }
            if (-not $semanticPass) { $callbackSemanticsPassed = $false }
        }
        Write-Host "HISTORICAL_CALLBACK name=$($case.Name) exit=$exitCode timeout=$(-not $finished) classification=$classification semantic_pass=$semanticPass"
        Write-Host $text
        if ($errors) { Write-Host $errors }
        $results += [ordered]@{
            name=$case.Name; selector=$case.Selector; expected_cases=$case.Count;
            guest_sha256=(Sha256 (Join-Path $runDir $case.Guest));
            exit_code=$exitCode; timed_out=(-not $finished); process_terminated=$process.HasExited;
            seconds=$watch.Elapsed.TotalSeconds; fixture_entered=$entered;
            missing_runtime_export=$missingExport; guest_success_marker=$guestPassed;
            loader_passed=$loaderPassed; callback_semantic_pass=$semanticPass;
            classification=$classification; stdout_sha256=(Sha256 $stdout); stderr_sha256=(Sha256 $stderr)
        }
        $process.Dispose()
    }
} finally {
    [Environment]::SetEnvironmentVariable('GESHER_CALLBACK_JUMP_CASE', $previousSelector)
}
if ($results.Count -ne 6) { $collectionPassed = $false; $callbackSemanticsPassed = $false }
$record = [ordered]@{
    schema='gesher-callback-pr120-baseline-run-v1'; source_commit=$commit; source_tree=$tree;
    build_receipt_sha256=(Sha256 $receiptPath); historical_runtime=$receipt.historical;
    sealed_bundle_artifact_id=$env:CALLBACK_BASELINE_ARTIFACT_ID;
    sealed_bundle_artifact_digest=$env:CALLBACK_BASELINE_ARTIFACT_DIGEST;
    run_id=$env:GITHUB_RUN_ID; run_attempt=$env:GITHUB_RUN_ATTEMPT;
    repository=$env:GITHUB_REPOSITORY; expected_runner=$env:CALLBACK_BASELINE_EXPECTED_RUNNER;
    actual_os=[Environment]::OSVersion.VersionString;
    image_os=$env:ImageOS; image_version=$env:ImageVersion;
    process_architecture=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString();
    preflight_passed=$preflightPassed; collection_passed=$collectionPassed;
    callback_semantics_passed=$callbackSemanticsPassed; results=$results;
    interpretation='Historical characterization only; successful collection is not a callback semantic pass. A timeout/crash after entry does not by itself identify its cause.'
}
$record | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $runDir 'baseline-run-receipt.json') -Encoding utf8
Write-Host "HISTORICAL_BASELINE_COLLECTION collection_passed=$collectionPassed callback_semantics_passed=$callbackSemanticsPassed"
if (-not $collectionPassed) { throw 'Historical baseline inconclusive: inspect setup, fixture-entry and raw process evidence' }
