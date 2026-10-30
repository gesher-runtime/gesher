param([Parameter(Mandatory)][string] $FixturePath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'receipt-diagnostics.ps1')

function Get-ReceiptConditions([string] $Text) {
  $tokens = $null; $errors = $null
  $ast = [Management.Automation.Language.Parser]::ParseInput(($Text -replace "`r`n", "`n"), [ref] $tokens, [ref] $errors)
  if ($errors.Count) { throw ($errors | Out-String) }
  @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.IfStatementAst] }, $true))
}

function Test-OriginalPredicate([string] $Expression, [object[]] $Rows, [object] $Expected, [string] $CaseKind) {
  $jobs = @($Rows | Where-Object name -eq $Expected.producer)
  Write-CiProducerIdentity -Producer $Expected.producer -Rows $jobs -Expected ([ordered]@{
    source = $Expected.source; run_id = $Expected.run_id; attempt = $Expected.attempt
    producer = $Expected.producer; allowed_conclusions = @('success'); required_count = 1
    scope = 'explicit prior-run context; global GITHUB values are unchanged'
    diagnostic_case = $CaseKind; synthetic_role_projection = $Expected.synthetic_role_projection
  })
  # Bind the old predicate's environment reads to an EXPLICIT local context.
  # No environment variable is changed and no full consumer script is executed.
  $bound = $Expression.Replace('$env:GITHUB_RUN_ID', '$Expected.run_id').Replace('$env:GITHUB_RUN_ATTEMPT', '$Expected.attempt').Replace('$env:GITHUB_SHA', '$Expected.source')
  if ($bound.Contains('$env:GITHUB')) { throw 'Unbound original identity input' }
  [bool](Invoke-Expression $bound)
}

$cases = @(Get-Content -LiteralPath $FixturePath -Raw | ConvertFrom-Json)
if ($cases.Count -ne 4) { throw 'Missing client predicate replay' }
foreach ($case in $cases) {
  $before = @(Get-ReceiptConditions $case.before)
  $after = @(Get-ReceiptConditions $case.after)
  $oldConditions = @($before | ForEach-Object { @($_.Clauses | ForEach-Object { $_.Item1.Extent.Text }) -join "`n" })
  $newConditions = @($after | ForEach-Object { @($_.Clauses | ForEach-Object { $_.Item1.Extent.Text }) -join "`n" })
  if ($oldConditions.Count -ne $newConditions.Count) { throw "Condition count changed: $($case.path)" }
  for ($i=0; $i -lt $oldConditions.Count; $i++) {
    if ($oldConditions[$i] -cne $newConditions[$i]) { throw "Original condition changed: $($case.path)" }
  }
  $target = @($after | Where-Object { $_.Extent.Text -match "throw 'Producer (identity mismatch|source/run/attempt mismatch)'" })
  if ($target.Count -ne 1) { throw "Missing/ambiguous original producer predicate: $($case.path)" }
  $expression = $target[0].Clauses[0].Item1.Extent.Text
  if (Test-OriginalPredicate $expression @($case.job) $case.expected 'declared_basis') { throw "Valid declared supplier rejected: $($case.path)" }
  foreach ($mutation in @(
    @{ field='head_sha'; value=('f' * 40) }, @{ field='run_id'; value=1 },
    @{ field='run_attempt'; value=2 }, @{ field='name'; value='wrong-role' },
    @{ field='conclusion'; value='failure' }, @{ field='head_sha'; value=$null }
  )) {
    $changed = $case.job | ConvertTo-Json -Depth 30 -Compress | ConvertFrom-Json
    $changed.($mutation.field) = $mutation.value
    if (-not (Test-OriginalPredicate $expression @($changed) $case.expected "counterfactual:$($mutation.field)")) {
      throw "Wrong $($mutation.field) accepted by original predicate: $($case.path)"
    }
  }
  $nonterminal = $case.job | ConvertTo-Json -Depth 30 -Compress | ConvertFrom-Json
  $nonterminal.status = 'in_progress'; $nonterminal.conclusion = $null
  if (-not (Test-OriginalPredicate $expression @($nonterminal) $case.expected 'counterfactual:nonterminal')) { throw 'Original nonterminal rejection changed' }
  if (-not (Test-OriginalPredicate $expression @($case.job, $case.job) $case.expected 'counterfactual:duplicate')) { throw 'Original duplicate-role rejection changed' }
  foreach ($field in @('head_sha','run_id','run_attempt')) {
    $missing = $case.job | ConvertTo-Json -Depth 30 -Compress | ConvertFrom-Json
    $missing.PSObject.Properties.Remove($field)
    if (-not (Test-OriginalPredicate $expression @($missing) $case.expected "counterfactual:missing-$field")) {
      throw "Missing $field accepted by original predicate"
    }
  }
  Write-Host "RECEIPT_POWERSHELL_PREDICATE_PASS path=$($case.path) synthetic_role_projection=$($case.synthetic_role_projection) predicates=UNCHANGED retry=NONE"
}
