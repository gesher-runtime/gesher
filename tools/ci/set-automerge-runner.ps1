[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('github','local')][string]$Target)
$ErrorActionPreference = 'Stop'
if ($Target -eq 'local') {
    $groups = gh api orgs/gesher-runtime/actions/runner-groups | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read runner groups.' }
    $group = @($groups.runner_groups | Where-Object name -eq 'gesher-automerge')
    if ($group.Count -ne 1 -or !$group[0].restricted_to_workflows -or $group[0].visibility -ne 'selected') { throw 'Restricted automerge group is required.' }
    $allowed = 'gesher-runtime/gesher/.github/workflows/automerge.yml@refs/heads/main'
    if ($group[0].selected_workflows -notcontains $allowed) { throw 'Trusted main workflow is not authorized.' }
    foreach ($workflow in $group[0].selected_workflows) {
        if ($workflow -cne $allowed -and $workflow -cnotmatch '^gesher-runtime/gesher/\.github/workflows/automerge-runner-probe\.yml@[0-9a-f]{40}$') { throw 'Unexpected runner workflow access.' }
    }
    $runners = gh api "orgs/gesher-runtime/actions/runner-groups/$($group[0].id)/runners" | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or @($runners.runners | Where-Object { $_.name -eq 'gesher-automerge-local' -and $_.status -eq 'online' }).Count -ne 1) { throw 'The dedicated runner must be online before enabling local routing.' }
}
$flag = if ($Target -eq 'local') { 'true' } else { 'false' }
gh variable set GESHER_AUTOMERGE_LOCAL --repo gesher-runtime/gesher --body $flag
if ($LASTEXITCODE -ne 0) { throw 'Could not update routing flag.' }
Write-Output "New trusted-main automerge jobs use: $Target. Already queued jobs retain their assigned runner."
