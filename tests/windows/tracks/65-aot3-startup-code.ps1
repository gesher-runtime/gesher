# The Gesher startup supplement comes from actual ART profiles on both runners.
# Independently inspect two framework paths absent from AOSP's original input.
$startupOat = NoDrive "$root/system/framework/x86_64/boot-framework.oat"
foreach ($selected in @(
    @{ Class = 'PackageParser2'; Method = 'parsePackage' },
    @{ Class = 'SystemFonts'; Method = 'buildSystemFallback' })) {
  $inspection = Aot2Run "$Dir/oatdump.exe" @("--oat-file=$startupOat",
    "--dex-file=$(NoDrive "$Dir/framework.jar")", "--class-filter=$($selected.Class)",
    "--method-filter=$($selected.Method)", '--no-disassemble') @{} 120
  Aot3Check "startup supplement compiles $($selected.Class).$($selected.Method) in the shipped framework oat" (
    $inspection.Exit -eq 0 -and $inspection.Out.Contains($selected.Class) -and
    $inspection.Out.Contains($selected.Method) -and
    $inspection.Out -match 'CODE:\s+\(code_offset=0x[0-9a-fA-F]+ size=[1-9][0-9]*\)') $inspection.Out
  ($inspection.Out -split "`n" | Where-Object { $_ -match 'PackageParser2|SystemFonts|CODE:' } |
    Select-Object -First 12) | Out-Host
}
