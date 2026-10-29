$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'WinRT compilation is CI only' }
. (Join-Path $PSScriptRoot '../tracks/geocoder-operation.ps1')
$inputPath = $env:GESHER_GEOCODER_COMPILER_INPUT
if ((Get-GeocoderSourceSha256 $inputPath) -cne $env:GESHER_GEOCODER_COMPILER_INPUT_SHA256) { throw 'Compiler input receipt mismatch' }
$config = [IO.File]::ReadAllText($inputPath) | ConvertFrom-Json
if ($config.SourceRevision -cne $env:GITHUB_SHA -or $config.RunId -cne $env:GITHUB_RUN_ID -or $config.RunAttempt -cne $env:GITHUB_RUN_ATTEMPT) { throw 'Compiler context mismatch' }
if ((Get-GeocoderSourceSha256 $PSCommandPath) -cne $config.WorkerSha256 -or [IO.Path]::GetFullPath($PSCommandPath) -cne $config.Worker) { throw 'Compiler worker identity mismatch' }
Write-Host "WINRT_COMPILER_HOST version=$($PSVersionTable.PSVersion) home=$PSHOME clr=$([Environment]::Version)"
if ($PSVersionTable.PSVersion.ToString() -cne '7.6.9' -or $config.PowerShell -cne '7.6.9') { throw 'Unpinned compiler host' }
$executable = [IO.Path]::GetFullPath((Join-Path $PSHOME 'pwsh.exe'))
if ($executable -cne $config.Compiler -or (Get-GeocoderSourceSha256 $executable) -cne $config.CompilerSha256) { throw 'Compiler host identity mismatch' }
foreach ($dependency in @($config.Compilers) + @($config.Sources) + @($config.References)) {
  if ((Get-GeocoderSourceSha256 $dependency.Path) -cne $dependency.Sha256) { throw 'Compiler dependency mismatch' }
}
# PowerShell bundles these exact Roslyn assemblies. No Add-Type, CodeDOM,
# csc.exe, cvtres.exe, shared compiler server, or external linker is invoked.
foreach ($entry in $config.Compilers) {
  $loaded = [Reflection.Assembly]::LoadFrom($entry.Path)
  if ($loaded.Location -cne $entry.Path -or $loaded.FullName -cne $entry.AssemblyName) { throw 'Roslyn load identity mismatch' }
}
$cancel = [Threading.CancellationTokenSource]::new()
$stream = $null
try {
  $cancel.CancelAfter(40000)
  $parse = [Microsoft.CodeAnalysis.CSharp.CSharpParseOptions]::Default.WithLanguageVersion([Microsoft.CodeAnalysis.CSharp.LanguageVersion]::CSharp5)
  $trees = [Collections.Generic.List[Microsoft.CodeAnalysis.SyntaxTree]]::new()
  foreach ($entry in $config.Sources) {
    # Hash the same bytes that are decoded and parsed.
    $bytes = [IO.File]::ReadAllBytes($entry.Path)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    if ($hash -cne $entry.Sha256) { throw 'Compiler source changed' }
    $trees.Add([Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree]::ParseText([Text.Encoding]::UTF8.GetString($bytes), $parse, $entry.Path, [Text.Encoding]::UTF8, $cancel.Token))
  }
  $references = [Collections.Generic.List[Microsoft.CodeAnalysis.MetadataReference]]::new()
  foreach ($entry in $config.References) {
    $references.Add([Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($entry.Path, [Microsoft.CodeAnalysis.MetadataReferenceProperties]::Assembly, $null))
  }
  # Select the pinned compiler's current constructor and explicitly establish
  # the defaults before overriding all build options relevant to this library.
  $constructors = @([Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions].GetConstructors() | Where-Object { $_.GetParameters().Count -eq 27 })
  if ($constructors.Count -ne 1) { throw 'Roslyn options contract changed' }
  $parameters = $constructors[0].GetParameters()
  $arguments = [object[]]::new($parameters.Count)
  $arguments[0] = [Microsoft.CodeAnalysis.OutputKind]::DynamicallyLinkedLibrary
  for ($i = 1; $i -lt $parameters.Count; $i++) {
    if (-not $parameters[$i].IsOptional) { throw 'Roslyn options no longer optional' }
    $arguments[$i] = $parameters[$i].DefaultValue
  }
  $options = $constructors[0].Invoke($arguments).WithOptimizationLevel([Microsoft.CodeAnalysis.OptimizationLevel]::Release).WithPlatform([Microsoft.CodeAnalysis.Platform]::AnyCpu).WithDeterministic($true).WithConcurrentBuild($false)
  $compilation = [Microsoft.CodeAnalysis.CSharp.CSharpCompilation]::Create('GesherGeocoderProbes', $trees, $references, $options)
  $stream = [IO.FileStream]::new($config.Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
  # Exactly the seven-argument API: no native resources, signing or PDB paths.
  $emit = @([Microsoft.CodeAnalysis.CSharp.CSharpCompilation].GetMethods() | Where-Object { $_.Name -eq 'Emit' -and $_.GetParameters().Count -eq 7 })
  if ($emit.Count -ne 1) { throw 'Roslyn emit contract changed' }
  $result = $emit[0].Invoke($compilation, [object[]]@($stream, $null, $null, $null, $null, $null, $cancel.Token))
  foreach ($diagnostic in $result.Diagnostics) { Write-Host $diagnostic.ToString() }
  if (-not $result.Success) { throw 'Roslyn compilation failed' }
  Write-Host "WINRT_ROSLYN_EMIT success=True external_compilers=False cancellation_ms=40000 language=CSharp5 target=NetFramework4 output=library optimize=Release platform=AnyCpu deterministic=True concurrent=False native_resources=False"
} finally {
  if ($stream) { $stream.Dispose() }
  $cancel.Dispose()
}
