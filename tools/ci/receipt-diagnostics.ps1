# Identity observations only. These functions do not accept, reject or retry.
function Write-CiProducerIdentity {
  param([string] $Producer, [AllowEmptyCollection()][object[]] $Rows,
        [Collections.IDictionary] $Expected = $null)
  if ($null -eq $Expected) {
    $Expected = [ordered]@{
      repository = $env:GITHUB_REPOSITORY; repository_id = $env:GITHUB_REPOSITORY_ID
      run_id = $env:GITHUB_RUN_ID; attempt = $env:GITHUB_RUN_ATTEMPT; source = $env:GITHUB_SHA
      producer = $Producer; required_count = 1; allowed_conclusions = @('success')
      workflow = $env:GITHUB_WORKFLOW
      status_policy = 'observed only; original predicate unchanged'
    }
  }
  $received = @($Rows | ForEach-Object {
    $row = $_
    [ordered]@{ id = $_.id; name = $_.name; workflow_name = $_.workflow_name; run_id = $_.run_id; run_attempt = $_.run_attempt
      head_sha = $_.head_sha; status = $_.status; conclusion = $_.conclusion
      started_at = $_.started_at; completed_at = $_.completed_at
      present_identity_fields = @('id','name','workflow_name','run_id','run_attempt','head_sha','status','conclusion','started_at','completed_at' | Where-Object { $_ -in $row.PSObject.Properties.Name }) }
  })
  $record = [ordered]@{ stage = 'producer'; expected = $Expected; received = $received; reason = 'observed' }
  Write-Host ('CI_RECEIPT_IDENTITY ' + ($record | ConvertTo-Json -Depth 12 -Compress))
}

function Write-CiArtifactIdentity {
  param([string] $Name, [object] $Artifact, [object] $Producer,
        [Collections.IDictionary] $Expected = $null)
  if ($null -eq $Expected) {
    $Expected = [ordered]@{
      repository = $env:GITHUB_REPOSITORY; repository_id = $env:GITHUB_REPOSITORY_ID
      head_repository_id = $env:GITHUB_REPOSITORY_ID; run_id = $env:GITHUB_RUN_ID
      attempt = $env:GITHUB_RUN_ATTEMPT; source = $env:GITHUB_SHA; name = $Name
      expired = $false; size_min = 1; digest_pattern = 'sha256:[0-9a-f]{64}'
      producer_id = $Producer.id; upload_not_before = $Producer.started_at
      upload_not_after = $Producer.completed_at
    }
  }
  $received = [ordered]@{
    id = $Artifact.id; name = $Artifact.name; expired = $Artifact.expired
    size_in_bytes = $Artifact.size_in_bytes; digest = $Artifact.digest; created_at = $Artifact.created_at
    workflow_run = [ordered]@{ id = $Artifact.workflow_run.id; head_sha = $Artifact.workflow_run.head_sha
      repository_id = $Artifact.workflow_run.repository_id; head_repository_id = $Artifact.workflow_run.head_repository_id }
    present_identity_fields = @('id','name','expired','size_in_bytes','digest','created_at','workflow_run' | Where-Object { $_ -in $Artifact.PSObject.Properties.Name })
    present_workflow_run_fields = @('id','head_sha','repository_id','head_repository_id' | Where-Object { $_ -in $Artifact.workflow_run.PSObject.Properties.Name })
  }
  $record = [ordered]@{ stage = 'artifact'; expected = $Expected; received = $received; reason = 'observed' }
  Write-Host ('CI_RECEIPT_IDENTITY ' + ($record | ConvertTo-Json -Depth 12 -Compress))
}

function Write-CiArtifactInventory {
  param([string] $Name, [object] $Inventory, [AllowEmptyCollection()][object[]] $Rows,
        [object] $Producer, [Collections.IDictionary] $Expected = $null)
  if ($null -eq $Expected) {
    $Expected = [ordered]@{ name = $Name; required_count = 1; source = $env:GITHUB_SHA
      run_id = $env:GITHUB_RUN_ID; attempt = $env:GITHUB_RUN_ATTEMPT
      repository = $env:GITHUB_REPOSITORY; repository_id = $env:GITHUB_REPOSITORY_ID
      head_repository_id = $env:GITHUB_REPOSITORY_ID; expired = $false; size_min = 1
      digest_pattern = 'sha256:[0-9a-f]{64}'; producer_id = $Producer.id
      upload_not_before = $Producer.started_at; upload_not_after = $Producer.completed_at }
  }
  $record = [ordered]@{ stage = 'artifact_inventory'; expected = $Expected; reason = 'observed'
    received = [ordered]@{ total_count = $Inventory.total_count; received_count = @($Inventory.artifacts).Count
      selected_count = @($Rows).Count; names = @($Inventory.artifacts | ForEach-Object { $_.name })
      present_inventory_fields = @('total_count','artifacts' | Where-Object { $_ -in $Inventory.PSObject.Properties.Name }) } }
  Write-Host ('CI_RECEIPT_IDENTITY ' + ($record | ConvertTo-Json -Depth 12 -Compress))
  foreach ($candidate in $Rows) {
    Write-CiArtifactIdentity -Name $Name -Artifact $candidate -Producer $Producer -Expected $Expected
  }
}
