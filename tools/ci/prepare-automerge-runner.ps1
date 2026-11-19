# Management computer only; uses its existing gh login. Output is public setup data.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$ProbeSHA,
    [string]$OutputDirectory=(Join-Path $env:TEMP 'gesher-automerge-setup')
)
$ErrorActionPreference='Stop'
function Read-GitHub([string]$Endpoint) {
    $data=gh api $Endpoint
    if ($LASTEXITCODE -ne 0) { throw "GitHub read failed: $Endpoint" }
    return ($data | ConvertFrom-Json)
}
function Write-GitHub([string]$Method,[string]$Endpoint,$Data) {
    $bodyFile=Join-Path $env:TEMP 'gesher-automerge-group-request.json'
    [IO.File]::WriteAllText($bodyFile,($Data | ConvertTo-Json -Depth 6),(New-Object Text.UTF8Encoding($false)))
    $reply=gh api --method $Method $Endpoint --input $bodyFile
    if ($LASTEXITCODE -ne 0) { throw "GitHub update failed: $Endpoint" }
    if ($reply) { return ($reply | ConvertFrom-Json) }
}
$repository=Read-GitHub 'repos/gesher-runtime/gesher'
if ($repository.full_name -cne 'gesher-runtime/gesher') { throw 'Unexpected repository.' }
$mainPolicy='gesher-runtime/gesher/.github/workflows/automerge.yml@refs/heads/main'
$probePolicy="gesher-runtime/gesher/.github/workflows/automerge-runner-probe.yml@$ProbeSHA"
$policies=@($mainPolicy,$probePolicy)
$groups=Read-GitHub 'orgs/gesher-runtime/actions/runner-groups'
$matches=@($groups.runner_groups | Where-Object name -eq 'gesher-automerge')
if ($matches.Count -gt 1) { throw 'Duplicate controller groups.' }
$spec=@{name='gesher-automerge';visibility='selected';allows_public_repositories=$true;restricted_to_workflows=$true;selected_workflows=$policies}
if ($matches.Count -eq 0) {
    $spec.selected_repository_ids=@($repository.id)
    $group=Write-GitHub POST 'orgs/gesher-runtime/actions/runner-groups' $spec
} else {
    $group=Read-GitHub "orgs/gesher-runtime/actions/runner-groups/$($matches[0].id)"
    if ($group.visibility -cne 'selected' -or !$group.restricted_to_workflows) { throw 'Refusing to reuse a broad controller group.' }
    foreach ($workflow in $group.selected_workflows) {
        if ($workflow -cne $mainPolicy -and $workflow -cnotmatch '^gesher-runtime/gesher/\.github/workflows/automerge-runner-probe\.yml@[0-9a-f]{40}$') { throw 'Unexpected existing workflow access.' }
    }
    $repos=Read-GitHub "orgs/gesher-runtime/actions/runner-groups/$($group.id)/repositories"
    if ($repos.total_count -ne 1 -or $repos.repositories[0].id -ne $repository.id) { throw 'Unexpected controller repository access.' }
    $group=Write-GitHub PATCH "orgs/gesher-runtime/actions/runner-groups/$($group.id)" $spec
}
$group=Read-GitHub "orgs/gesher-runtime/actions/runner-groups/$($group.id)"
if (!$group.restricted_to_workflows -or $group.visibility -cne 'selected' -or @($group.selected_workflows).Count -ne 2 -or @($policies | Where-Object { $group.selected_workflows -cnotcontains $_ }).Count) { throw 'Controller group verification failed.' }
$downloads=Read-GitHub 'orgs/gesher-runtime/actions/runners/downloads'
$runner=@($downloads | Where-Object { $_.os -eq 'linux' -and $_.architecture -eq 'x64' })
if ($runner.Count -ne 1 -or $runner[0].sha256_checksum -cnotmatch '^[0-9a-f]{64}$') { throw 'Unexpected official runner download.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
foreach ($name in @('setup-automerge-vm.ps1','install-automerge-runner.sh')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $OutputDirectory $name) }
$package=[ordered]@{schema_version=1;repository=$repository.full_name;runner_group='gesher-automerge';runner_group_id=$group.id;allowed_workflows=$policies;download_url=$runner[0].download_url;sha256=$runner[0].sha256_checksum}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'automerge-package.json'),($package | ConvertTo-Json -Depth 5),(New-Object Text.UTF8Encoding($false)))
Write-Output "Public setup package: $OutputDirectory"
Write-Output "Restricted controller group: $($group.id)"
Write-Output 'No personal credentials or registration token are included.'
