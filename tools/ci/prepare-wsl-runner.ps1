# Run on the management computer only. Never copies account credentials.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$repoName = 'gesher-runtime/gesher'
$orgName = 'gesher-runtime'
$groupName = 'gesher-build-trial'
$workflow = "$repoName/.github/workflows/arm64-windows-self-hosted.yml@refs/heads/main"

$allowedWorkflows = @(
    $workflow,
    "$repoName/.github/workflows/framework.yml@93239cc6ea2702ff5f97c7bbe7c8dd64ea6110c7"
)

function Invoke-GhApi {
    param([string]$Endpoint, [string]$Method = 'GET', [hashtable]$Body)
    if ($null -ne $Body) {
        $result = $Body | ConvertTo-Json -Depth 10 -Compress |
            & gh api --method $Method $Endpoint --input -
    } else {
        $result = & gh api --method $Method $Endpoint
    }
    if ($LASTEXITCODE -ne 0) { throw "GitHub API failed: $Method $Endpoint" }
    return ($result | ConvertFrom-Json)
}

Get-Command gh -ErrorAction Stop | Out-Null
& gh auth status
if ($LASTEXITCODE -ne 0) { throw 'Run gh auth login first.' }

# A label alone does not prevent an external PR from targeting this machine.
# Create a server-enforced group before issuing any registration credentials.
$repoInfo = Invoke-GhApi "repos/$repoName"
$publishedWorkflow = Invoke-GhApi "repos/$repoName/contents/.github/workflows/arm64-windows-self-hosted.yml?ref=main"
if ($publishedWorkflow.type -ne 'file') {
    throw 'Merge the trial workflow into main before registering this runner.'
}
$groups = Invoke-GhApi "orgs/$orgName/actions/runner-groups?per_page=100"
if ($groups.total_count -gt 100) { throw 'Too many runner groups; inspect the policy manually.' }
$matchingGroups = @($groups.runner_groups | Where-Object { $_.name -eq $groupName })
if ($matchingGroups.Count -gt 1) { throw 'Runner group name is ambiguous.' }
if ($matchingGroups.Count -eq 0) {
    $group = Invoke-GhApi "orgs/$orgName/actions/runner-groups" 'POST' @{
        name = $groupName
        visibility = 'selected'
        selected_repository_ids = @([long]$repoInfo.id)
        allows_public_repositories = $true
        restricted_to_workflows = $true
        selected_workflows = $allowedWorkflows
    }
} else {
    $group = $matchingGroups[0]
}
$group = Invoke-GhApi "orgs/$orgName/actions/runner-groups/$($group.id)"
$selectedRepos = Invoke-GhApi "orgs/$orgName/actions/runner-groups/$($group.id)/repositories?per_page=100"
if ($group.visibility -ne 'selected' -or
    $group.allows_public_repositories -ne $true -or
    $group.restricted_to_workflows -ne $true -or
    @($group.selected_workflows | Where-Object { $_ -cnotin $allowedWorkflows }).Count -ne 0 -or
    $selectedRepos.total_count -ne 1 -or
    $selectedRepos.repositories[0].id -ne $repoInfo.id) {
    throw 'Runner group policy is not restrictive enough. No runner was registered. Do not use the Default group as a fallback.'
}

$missingWorkflows = @($allowedWorkflows | Where-Object { $_ -cnotin @($group.selected_workflows) })
if ($missingWorkflows.Count) {
    $group = Invoke-GhApi "orgs/$orgName/actions/runner-groups/$($group.id)" 'PATCH' @{
        restricted_to_workflows = $true
        selected_workflows = $allowedWorkflows
    }
}
if (@($group.selected_workflows).Count -ne $allowedWorkflows.Count -or
    @($allowedWorkflows | Where-Object { $_ -cnotin @($group.selected_workflows) }).Count) {
    throw 'GitHub did not return the exact approved workflow allowlist.'
}

$release = Invoke-GhApi 'repos/actions/runner/releases/latest'
$assets = @($release.assets | Where-Object { $_.name -match '^actions-runner-linux-x64-[0-9.]+\.tar\.gz$' })
if ($assets.Count -ne 1 -or $assets[0].digest -notmatch '^sha256:[0-9a-f]{64}$' -or
    $assets[0].browser_download_url -notmatch '^https://github.com/actions/runner/releases/download/v[0-9.]+/actions-runner-linux-x64-[0-9.]+\.tar\.gz$') {
    throw 'Official runner download URL or SHA-256 digest is unavailable.'
}


New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$package = @{
    schema_version = 1
    repository = $repoName
    organization = $orgName
    runner_group = $groupName
    runner_group_id = [long]$group.id
    allowed_workflow = $workflow
    allowed_workflows = $allowedWorkflows
    download_url = $assets[0].browser_download_url
    sha256 = $assets[0].digest.Substring(7)
}
$packagePath = Join-Path $OutputDirectory 'runner-package.json'
[IO.File]::WriteAllText($packagePath, ($package | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'setup-wsl-runner.ps1') -Destination (Join-Path $OutputDirectory 'setup-wsl-runner.ps1') -Force
Write-Host "Prepared: $OutputDirectory"
Write-Host 'Copy BOTH files to the runner computer. They contain no credentials.'
Write-Host 'No registration token has been generated. Generate it when the second computer prompts for it.'
