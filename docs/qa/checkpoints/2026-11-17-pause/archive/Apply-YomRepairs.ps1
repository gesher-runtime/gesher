#requires -Version 5.1
<#
Apply existing palmoni5 repair commits to four contributor PR branches.
Default: verify and show the plan only. -Apply performs fast-forward pushes.
No checkout, reset, rebase, new commit, review, label, or merge into main.
PR #93 remains blocked by its separate substantive review after this fixture fix.
This script was statically reviewed; it was not executed against GitHub here.
#>
[CmdletBinding()]
param(
    [string] $RepositoryPath = (Get-Location).Path,
    [switch] $Apply
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$repository = 'gesher-runtime/gesher'
$upstream = 'https://github.com/gesher-runtime/gesher.git'
$fork = 'https://github.com/yom-chadash/gesher.git'
$repoRoot = (Resolve-Path -LiteralPath $RepositoryPath).Path
$gitAuth = @('-c', 'credential.helper=', '-c', 'credential.https://github.com.helper=!gh auth git-credential')

function Invoke-CheckedGh {
    param([Parameter(Mandatory=$true)][string[]] $Arguments)
    & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI failed. No further changes will be made.' }
}
function Invoke-CheckedGit {
    param([Parameter(Mandatory=$true)][string[]] $Arguments)
    & git @gitAuth -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Git failed. No further changes will be made.' }
}
function Read-Pr {
    param($Item)
    $raw = Invoke-CheckedGh -Arguments @('api', '--hostname', 'github.com', "repos/$repository/pulls/$($Item.Number)")
    $pr = ($raw -join "`n") | ConvertFrom-Json
    if ($pr.state -ne 'open' -or $pr.draft -or $pr.merged) {
        throw "PR #$($Item.Number) is closed, merged, or a draft. Stop and recheck."
    }
    if ($null -eq $pr.head.repo -or $pr.head.repo.full_name -ne 'yom-chadash/gesher' -or $pr.head.ref -ne $Item.Source) {
        throw "PR #$($Item.Number) source repository/branch changed."
    }
    return $pr
}

$items = @(
    [pscustomobject]@{
        Number=87; Source='codex/56-httpengine-http2-policy-20261117'
        Expected='c0b2a744dfa1be80eabd22cfb29ea99d20656b23'
        Repair='503eaf20885481c4f860d9bde611c75a82823448'
        Temporary='qa/pr-87-http2-policy-20261117'
    },
    [pscustomobject]@{
        Number=92; Source='fix/56-external-url-result-20261117'
        Expected='80265b33640c046b3b7147ff6e65ba082e8991b1'
        Repair='09627a9a1964ceb863a8eb46d33b458b11db6a00'
        Temporary='qa/pr-92-external-url-results-20261117'
    },
    [pscustomobject]@{
        Number=93; Source='fix/56-bionic-memory-lock-20261117'
        Expected='1df606ebdc780bc376156e63c2e3a96fef350138'
        Repair='4e53dd7a46191d75df1bd1914389f599fe4b1828'
        Temporary='qa/pr-93-working-set-fixture-20261117'
    },
    [pscustomobject]@{
        Number=94; Source='fix/56-codec-output-timestamps-20261117'
        Expected='cc7656eaeb0b32b1b81d6bd264b524de1ebec00f'
        Repair='78730c164966b0a288e9b65ae0b7e9ca5612a335'
        Temporary='qa/pr-94-explicit-timestamps-20261117'
    }
)

$login = ((Invoke-CheckedGh -Arguments @('api','--hostname','github.com','user','--jq','.login')) -join "`n").Trim()
if ($login -ne 'palmoni5') { throw "Expected palmoni5; active GitHub account is $login." }
Invoke-CheckedGit -Arguments @('rev-parse','--git-dir') | Out-Null

# Verify every planned update before performing any push.
foreach ($item in $items) {
    $pr = Read-Pr -Item $item
    if ($pr.head.sha -ne $item.Expected -and $pr.head.sha -ne $item.Repair) {
        throw "PR #$($item.Number) has a newer/different head $($pr.head.sha). Nothing has been pushed."
    }
    if ($pr.head.sha -eq $item.Repair) {
        Write-Host "PR #$($item.Number): repair already applied; cleanup can be checked."
        continue
    }
    if (-not $pr.maintainer_can_modify) { throw "Maintainer edits disabled for PR #$($item.Number)." }
    Invoke-CheckedGit -Arguments @('fetch','--no-tags',$upstream,"refs/heads/$($item.Temporary)")
    $fetched = ((Invoke-CheckedGit -Arguments @('rev-parse','FETCH_HEAD')) -join "`n").Trim()
    if ($fetched -ne $item.Repair) { throw "Temporary repair branch changed for #$($item.Number)." }
    Invoke-CheckedGit -Arguments @('merge-base','--is-ancestor',$item.Expected,$item.Repair)
    if ((Read-Pr -Item $item).head.sha -ne $item.Expected) {
        throw "PR #$($item.Number) changed during preflight. Nothing has been pushed."
    }
    Write-Host "PR #$($item.Number): verified $($item.Expected) -> $($item.Repair)"
}
if (-not $Apply) {
    Write-Host 'Verification only. Add -Apply to push these four prepared repairs.'
    return
}

$completed = New-Object 'System.Collections.Generic.List[int]'
try {
    foreach ($item in $items) {
        $pr = Read-Pr -Item $item
        if ($pr.head.sha -ne $item.Repair) {
            if ($pr.head.sha -ne $item.Expected -or -not $pr.maintainer_can_modify) {
                throw "PR #$($item.Number) changed before its push. Stop; preserve the newer work."
            }
            # A normal fast-forward push preserves the contributor's history.
            Invoke-CheckedGit -Arguments @('push',$fork,"$($item.Repair):refs/heads/$($item.Source)")
        }
        if ((Read-Pr -Item $item).head.sha -ne $item.Repair) {
            throw "PR #$($item.Number) update not confirmed. Keep its temporary branch."
        }
        $actual = @(Invoke-CheckedGit -Arguments @('ls-remote','--heads',$fork,"refs/heads/$($item.Source)"))
        if ($actual.Count -ne 1 -or ($actual[0] -split '\s+')[0] -ne $item.Repair) {
            throw "PR #$($item.Number) source branch changed before cleanup."
        }
        $temporary = @(Invoke-CheckedGit -Arguments @('ls-remote','--heads',$upstream,"refs/heads/$($item.Temporary)"))
        if ($temporary.Count -gt 0) {
            if ($temporary.Count -ne 1 -or ($temporary[0] -split '\s+')[0] -ne $item.Repair) {
                throw "Temporary branch for #$($item.Number) changed; it was not deleted."
            }
            if ((Read-Pr -Item $item).head.sha -ne $item.Repair) {
                throw "PR #$($item.Number) changed. Keep its temporary branch."
            }
            # This lease only protects deletion of the EXACT temporary branch.
            # No forced update is used on a contributor branch.
            Invoke-CheckedGit -Arguments @('push',"--force-with-lease=refs/heads/$($item.Temporary):$($item.Repair)",$upstream,":refs/heads/$($item.Temporary)")
        }
        $completed.Add($item.Number)
        Write-Host "PR #$($item.Number) updated; temporary branch removed or already absent."
    }
} catch {
    Write-Warning "Completed PR updates before this stop: $($completed -join ', ')"
    throw
}
Write-Host 'No PR was approved, labeled, or merged by this script.'
Write-Host 'PR #93 still requires the substantive lock-accounting/lifecycle revision.'
