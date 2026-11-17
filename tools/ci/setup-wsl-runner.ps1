# Run on the runner computer. No gh executable or account login is needed.
[CmdletBinding()]
param(
    [string]$PackagePath = '',
    [string]$Distribution = 'Ubuntu-24.04',
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$RunnerName = 'gesher-wsl-trial'
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($PackagePath)) {
    # Resolve this in the script body: PowerShell 5.1 may not initialize
    # PSScriptRoot while parameter default expressions are evaluated.
    $PackagePath = Join-Path $PSScriptRoot 'runner-package.json'
}
Get-Command wsl.exe -ErrorAction Stop | Out-Null
$package = Get-Content -LiteralPath $PackagePath -Raw | ConvertFrom-Json
if ($package.schema_version -ne 1 -or
    $package.repository -cne 'gesher-runtime/gesher' -or
    $package.organization -cne 'gesher-runtime' -or
    $package.runner_group -cne 'gesher-build-trial' -or
    $package.allowed_workflow -cne 'gesher-runtime/gesher/.github/workflows/arm64-windows-self-hosted.yml@refs/heads/main' -or
    $package.runner_group_id -le 0 -or
    $package.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $package.download_url -cnotmatch '^https://github.com/actions/runner/releases/download/v[0-9.]+/actions-runner-linux-x64-[0-9.]+\.tar\.gz$') {
    throw 'Invalid public setup package. Regenerate it on the management computer.'
}

$linuxHome = ((& wsl.exe -d $Distribution -- printenv HOME) -join '').Trim()
if ($LASTEXITCODE -ne 0 -or $linuxHome -notmatch '^/home/[A-Za-z0-9_.-]+$') {
    throw 'WSL must start with a regular Linux user, not root.'
}
$architecture = ((& wsl.exe -d $Distribution -- uname -m) -join '').Trim()
if ($LASTEXITCODE -ne 0 -or $architecture -ne 'x86_64') { throw 'An x64 WSL distribution is required.' }

& wsl.exe -d $Distribution -- test -f "$linuxHome/gesher-actions-runner/.runner"
if ($LASTEXITCODE -eq 0) {
    throw 'Runner is already configured. Resume it with: wsl -d Ubuntu-24.04 -- bash -lc "cd ~/gesher-actions-runner && ./run.sh"'
}
if ($LASTEXITCODE -ne 1) { throw 'Could not check existing runner configuration.' }

Write-Host 'Installing build dependencies in WSL (this may take several minutes)...'
& wsl.exe -d $Distribution -u root -- apt-get update
if ($LASTEXITCODE -ne 0) { throw 'apt-get update failed.' }
& wsl.exe -d $Distribution -u root -- env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends git curl ca-certificates tar xz-utils python3 cmake ninja-build ccache libicu74 libssl3t64 libkrb5-3 zlib1g
if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed.' }

$download = @'
#!/usr/bin/env bash
set -euo pipefail
cd "$HOME/gesher-actions-runner"
curl --fail --location --retry 3 '__URL__' --output runner.tar.gz
echo '__DIGEST__  runner.tar.gz' | sha256sum --check --strict
tar -xzf runner.tar.gz
'@
$setup = @'
#!/usr/bin/env bash
set -euo pipefail
cd "$HOME/gesher-actions-runner"
read -r registration_token
registration_token=${registration_token%$'\r'}
./config.sh --unattended --url https://github.com/gesher-runtime \
  --token "$registration_token" --runnergroup gesher-build-trial \
  --name '__NAME__' --work _work
unset registration_token
exec ./run.sh
'@
$download = $download.Replace('__URL__', $package.download_url).
    Replace('__DIGEST__', $package.sha256).Replace("`r", '')
$setup = $setup.Replace('__NAME__', $RunnerName).Replace("`r", '')
$encodedDownload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($download))
$encodedSetup = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($setup))
& wsl.exe -d $Distribution -- bash -c "set -e; umask 077; mkdir -p ~/gesher-actions-runner; echo $encodedDownload | base64 -d > ~/gesher-actions-runner/download-trial.sh; echo $encodedSetup | base64 -d > ~/gesher-actions-runner/start-trial.sh"
if ($LASTEXITCODE -ne 0) { throw 'Could not write the runner setup scripts.' }
& wsl.exe -d $Distribution -- bash "$linuxHome/gesher-actions-runner/download-trial.sh"
if ($LASTEXITCODE -ne 0) { throw 'Runner download, checksum verification or extraction failed.' }

Write-Host 'Ready for registration. On the MANAGEMENT computer, run:'
Write-Host 'gh api --method POST orgs/gesher-runtime/actions/runners/registration-token --jq .token'
$secureToken = Read-Host 'Paste the temporary registration token here (hidden)' -AsSecureString
$tokenPointer = [IntPtr]::Zero
$plainToken = $null
Write-Host 'Starting runner. Keep this PowerShell window open; Ctrl+C stops it.'
try {
    $tokenPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureToken)
    $plainToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($tokenPointer)
    if ([string]::IsNullOrWhiteSpace($plainToken) -or $plainToken -match '[\r\n]') {
        throw 'A single registration token is required.'
    }
    # No token in command history, script files or the WSL command line.
    $plainToken | & wsl.exe -d $Distribution -- bash "$linuxHome/gesher-actions-runner/start-trial.sh"
    if ($LASTEXITCODE -ne 0) { throw 'Runner registration or execution failed.' }
} finally {
    $plainToken = $null
    if ($tokenPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($tokenPointer)
    }
    $secureToken.Dispose()
}
