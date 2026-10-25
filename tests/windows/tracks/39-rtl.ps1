# Right to left, further scripts and the locale from Windows (docs/fonts.md,
# ADR 0044). Dot-sourced by run-tests.ps1 after 34-windows.ps1, whose
# WindowAppCheck and $dvmWindows it uses (and WindowChecks.ps1's GesherWin32 and
# WaitForLog), and after 32-fonts.ps1, which installs the system fonts and
# points $GESHER_WINDOWS_FONTS at Windows' fonts.
#
# 1. LocaleTest (graphics-tests.dex): the locale list, first day of the week,
#    24-hour time and number format SystemLocale gives, against Windows' own
#    settings: as the runner has them, then with the regional format set to
#    en-GB (same language as the display: its region is taken) and to de-DE
#    (another language: its first day and hour cycle become -u-fw/-u-hc), each
#    restored after.
# 2. rtlapp.apk (tests/apps/rtlapp) in a hidden window, read back with
#    PrintWindow: Hebrew and Arabic TextViews (colored runs drawn right to
#    left), mixed bidi text, Arabic letters joined, alignment by textDirection,
#    textAlignment and gravity, CJK and emoji glyphs (not the .notdef box), a
#    layout mirrored when the locale is right to left. Three runs: the
#    runner's locale (en-US) in a single process; $GESHER_LOCALES=he-IL in a
#    single process, with the cursor moved through the RTL EditText by arrow
#    keys; ar-EG on the Gesher device (the app process gets the system
#    server's configuration).
if (-not ('GesherRtl' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class GesherRtl {
  static int At(int[] px, int x, int y) { return px[2 + y * px[0] + x]; }
  static bool Near(int c, int want, int tol) {
    return Math.Abs(((c >> 16) & 255) - ((want >> 16) & 255)) <= tol &&
           Math.Abs(((c >> 8) & 255) - ((want >> 8) & 255)) <= tol &&
           Math.Abs((c & 255) - (want & 255)) <= tol;
  }
  static bool Dark(int c) { return ((c >> 16) & 255) < 128 && ((c >> 8) & 255) < 128 && (c & 255) < 128; }
  static bool Ink(int c) { return ((c >> 16) & 255) < 200 || ((c >> 8) & 255) < 200 || (c & 255) < 200; }
  // The mean x of the pixels near `want` in the rectangle, -1 if none.
  public static double CenterX(int[] px, int x0, int y0, int w, int h, int want) {
    long sum = 0, n = 0;
    for (int y = y0; y < y0 + h && y < px[1]; y++)
      for (int x = x0; x < x0 + w && x < px[0]; x++)
        if (Near(At(px, x, y), want, 60)) { sum += x; n++; }
    return n < 10 ? -1 : (double) sum / n - x0;
  }
  // {first, last} column (relative to x0) with any non-white pixel, {-1, -1} if none.
  public static int[] InkColumns(int[] px, int x0, int y0, int w, int h) {
    int first = -1, last = -1;
    for (int x = x0; x < x0 + w && x < px[0]; x++)
      for (int y = y0; y < y0 + h && y < px[1]; y++)
        if (Ink(At(px, x, y))) { if (first < 0) first = x - x0; last = x - x0; break; }
    return new int[] { first, last };
  }
  public static int DarkCount(int[] px, int x0, int y0, int w, int h) {
    int n = 0;
    for (int y = y0; y < y0 + h && y < px[1]; y++)
      for (int x = x0; x < x0 + w && x < px[0]; x++)
        if (Dark(At(px, x, y))) n++;
    return n;
  }
  // Pixels clearly not gray (a color emoji's).
  public static int Colorful(int[] px, int x0, int y0, int w, int h) {
    int n = 0;
    for (int y = y0; y < y0 + h && y < px[1]; y++)
      for (int x = x0; x < x0 + w && x < px[0]; x++) {
        int c = At(px, x, y);
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        if (Math.Max(r, Math.Max(g, b)) - Math.Min(r, Math.Min(g, b)) > 80) n++;
      }
    return n;
  }
  // Shapes of dark pixels (8-connected) in the rectangle.
  public static int Components(int[] px, int x0, int y0, int w, int h) {
    var seen = new bool[w * h];
    var stack = new Stack<int>();
    int n = 0;
    for (int i = 0; i < w * h; i++) {
      int ix = i % w, iy = i / w;
      if (seen[i] || x0 + ix >= px[0] || y0 + iy >= px[1] || !Dark(At(px, x0 + ix, y0 + iy))) continue;
      n++;
      seen[i] = true;
      stack.Push(i);
      while (stack.Count > 0) {
        int p = stack.Pop(), x = p % w, y = p / w;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || x0 + nx >= px[0] || y0 + ny >= px[1]) continue;
            int q = ny * w + nx;
            if (!seen[q] && Dark(At(px, x0 + nx, y0 + ny))) { seen[q] = true; stack.Push(q); }
          }
      }
    }
    return n;
  }
}
'@
}

# --- 1. The locale from Windows -------------------------------------------------------

# What Windows says, in a new process (a regional format just set is seen by
# processes started after it): the preferred UI languages, the regional format,
# its first day of the week (as java.util.Calendar numbers it), whether its
# time format counts 24 hours, 1234567.5 in it.
function WindowsLocaleNow {
  $script = Join-Path $env:RUNNER_TEMP 'rtl-windows-locale.ps1'
  Set-Content $script @'
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class Mui {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
  static extern bool GetUserPreferredUILanguages(uint flags, out uint count, char[] buf, ref uint size);
  public static string Languages() {
    uint count, size = 0;
    GetUserPreferredUILanguages(8, out count, null, ref size);
    var buf = new char[size];
    GetUserPreferredUILanguages(8, out count, buf, ref size);
    return string.Join(",", new string(buf).Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries));
  }
}
"@
$c = [Globalization.CultureInfo]::new((Get-Culture).Name, $true)
[ordered]@{ ui = [Mui]::Languages(); format = $c.Name; firstDay = [int]$c.DateTimeFormat.FirstDayOfWeek + 1;
  is24 = $c.DateTimeFormat.ShortTimePattern.Contains('H'); number = (1234567.5).ToString('#,##0.###', $c) } | ConvertTo-Json -Compress
'@
  $json = & pwsh -NoProfile -File $script
  return $json | ConvertFrom-Json
}

# Sets the user's regional format (Set-Culture) for the processes started after it.
function SetRegionalFormat([string] $Name) {
  & powershell.exe -NoProfile -Command "Set-Culture $Name" 2>&1 | Out-Null
}

$win = WindowsLocaleNow
Write-Host "INFO  locale: Windows' display languages $($win.ui), regional format $($win.format), first day $($win.firstDay), 24h $($win.is24), 1234567.5 = $($win.number)"
# The display languages, the first taking the regional format's region when it is
# the same language (en-US with en-GB formats is en-GB); regional preferences
# (-u-) are compared too only when given.
function ExpectedLocales($win) {
  $tags = @($win.ui -split ',')
  $first = [Globalization.CultureInfo]::new($tags[0])
  $format = [Globalization.CultureInfo]::new($win.format)
  if ($first.TwoLetterISOLanguageName -eq $format.TwoLetterISOLanguageName -and $win.format -match '-([A-Z]{2})$') {
    $tags[0] = ($tags[0] -replace '-[A-Z]{2}$', '') + "-$($Matches[1])"
  }
  return ($tags | Select-Object -Unique) -join ','
}
$savedLocales = $env:GESHER_LOCALES
Remove-Item Env:GESHER_LOCALES -ErrorAction SilentlyContinue
GraphicsDex "locale from Windows: the display languages $($win.ui) with the regional format $($win.format) (first day $($win.firstDay), 24-hour $($win.is24))" `
  LocaleTest @((ExpectedLocales $win), "$($win.firstDay)", "$($win.is24)".ToLower(), $win.number)
$original = $win.format
try {
  # The same language as the display (en-US): its region is taken, en-GB's own
  # first day (Monday) and 24-hour time come with it, and its number format.
  SetRegionalFormat 'en-GB'
  $gb = WindowsLocaleNow
  if ($gb.format -ne 'en-GB') {
    Write-Host "INFO  locale: Set-Culture en-GB did not take ($($gb.format)); skipped"
  } elseif (($win.ui -split ',')[0] -notmatch '^en-') {
    Write-Host "INFO  locale: the display language is not English; en-GB skipped"
  } else {
    GraphicsDex 'locale from Windows: the regional format en-GB under an English display gives en-GB (Monday, 24-hour, its numbers)' `
      LocaleTest @((ExpectedLocales $gb), "$($gb.firstDay)", "$($gb.is24)".ToLower(), $gb.number)
  }
  # Another language (de-DE under an en-US display): the display language stays,
  # with de-DE's first day and hour cycle as regional preferences.
  SetRegionalFormat 'de-DE'
  $de = WindowsLocaleNow
  if ($de.format -ne 'de-DE' -or ($de.ui -split ',')[0] -ne 'en-US') {
    Write-Host "INFO  locale: not an en-US display with de-DE formats ($($de.ui), $($de.format)); skipped"
  } else {
    # (and Celsius, -u-mu-: de-DE's metric measurement system; locale track, ADR 0049)
    GraphicsDex 'locale from Windows: the regional format de-DE under an en-US display gives en-US-u-fw-mon-hc-h23-mu-celsius (Monday, 24-hour, Celsius)' `
      LocaleTest @("en-US-u-fw-mon-hc-h23-mu-celsius$((($de.ui -split ',') | Select-Object -Skip 1 | ForEach-Object { ",$_" }) -join '')", '2', 'true')
  }
} finally {
  SetRegionalFormat $original
  $back = WindowsLocaleNow
  Write-Host "INFO  locale: regional format restored to $($back.format)"
}
# $GESHER_LOCALES replaces the list as it is.
$env:GESHER_LOCALES = 'he-IL,en-US'
GraphicsDex 'locale from $GESHER_LOCALES (he-IL,en-US)' LocaleTest @('he-IL,en-US', '1', "$($win.is24)".ToLower())
Remove-Item Env:GESHER_LOCALES

# --- 2. rtlapp.apk in a window --------------------------------------------------------

# The views' rectangles RtlActivity logged: name -> @(x, y, w, h).
function RtlViews([string] $Text) {
  $views = @{}
  foreach ($m in [regex]::Matches($Text, 'RtlActivity view (\w+) (-?\d+) (-?\d+) (\d+) (\d+)')) {
    $views[$m.Groups[1].Value] = @([int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value, [int]$m.Groups[5].Value)
  }
  return $views
}

# Text ink ({first, last} column) against a view's rectangle: at its right or left
# edge (glyphs' side bearings leave a few pixels).
function AlignedRight($ink, $rect) { return $ink[0] -ge 0 -and $ink[1] -ge $rect[2] - 12 -and $ink[0] -gt 40 }
function AlignedLeft($ink, $rect) { return $ink[0] -ge 0 -and $ink[0] -le 12 -and $ink[1] -lt $rect[2] - 40 }

# Problems with rtlapp's window ($px) given its log; $Rtl: the layout is mirrored.
function RtlAppPixels($px, [string] $Text, [bool] $Rtl, [string] $Resources) {
  $out = @()
  $v = RtlViews $Text
  foreach ($n in 'mirror', 'forced', 'hebrew', 'arabic', 'mixed', 'joined', 'isolated', 'latin_rtl', 'view_start', 'gravity_end', 'cjk', 'tofu', 'emoji') {
    if (-not $v.ContainsKey($n)) { return @("no position for view $n in the log") }
  }
  $dir = if ($Rtl) { 'rtl' } else { 'ltr' }
  if ($Text -notmatch "RtlActivity locales [^\n]*layout direction $dir, root $dir, resources $Resources, supportsRtl true") {
    $out += "configuration: not layout direction $dir with resources $Resources"
  }
  # Rows of a red and a blue half: the first mirrored with the layout, the second
  # forced left to right.
  $m = $v['mirror']; $f = $v['forced']
  $redLeft = if ($Rtl) { 0x0000ff } else { 0xff0000 }
  $out += @(Regions $px @(@('mirror row, left half', $m[0], $m[1], $m[0] + 200, $m[1] + $m[3], $redLeft),
                          @('mirror row, right half', $m[0] + 200, $m[1], $m[0] + 400, $m[1] + $m[3], ($redLeft -bxor 0xff00ff)),
                          @('forced LTR row, left half', $f[0], $f[1], $f[0] + 200, $f[1] + $f[3], 0xff0000),
                          @('forced LTR row, right half', $f[0] + 200, $f[1], $f[0] + 400, $f[1] + $f[3], 0x0000ff)))
  # Colored runs: in logical order red then blue (Hebrew, Arabic); red, green, magenta,
  # blue mixing Hebrew with "ABC 123": drawn right to left, an LTR run inside.
  foreach ($n in 'hebrew', 'arabic', 'mixed') {
    $r = $v[$n]
    $x = @{}
    foreach ($c in @(@('red', 0xff0000), @('green', 0x00a000), @('magenta', 0xff00ff), @('blue', 0x0000ff))) {
      $x[$c[0]] = [GesherRtl]::CenterX($px, $r[0], $r[1], $r[2], $r[3], $c[1])
    }
    $order = if ($n -eq 'mixed') { @('blue', 'green', 'magenta', 'red') } else { @('blue', 'red') }
    $xs = $order | ForEach-Object { [math]::Round($x[$_]) }
    Write-Host "INFO  ${n}: runs from left to right $($order -join ', ') at x $($xs -join ', ')"
    for ($i = 0; $i -lt $order.Count; $i++) {
      if ($x[$order[$i]] -lt 0) { $out += "${n}: no $($order[$i]) text" }
      elseif ($i -gt 0 -and $x[$order[$i]] -le $x[$order[$i - 1]]) { $out += "${n}: $($order[$i]) not right of $($order[$i - 1])" }
    }
    $ink = [GesherRtl]::InkColumns($px, $r[0], $r[1], $r[2], $r[3])
    if (-not (AlignedRight $ink $r)) { $out += "${n}: an RTL paragraph with gravity start is not aligned right (ink $($ink -join '..') of $($r[2]))" }
  }
  # Arabic letters joined (one connected shape) against the same letters kept apart
  # by ZWNJ.
  $j = $v['joined']; $s = $v['isolated']
  $jc = [GesherRtl]::Components($px, $j[0], $j[1], $j[2], $j[3]); $sc = [GesherRtl]::Components($px, $s[0], $s[1], $s[2], $s[3])
  $ji = [GesherRtl]::InkColumns($px, $j[0], $j[1], $j[2], $j[3]); $si = [GesherRtl]::InkColumns($px, $s[0], $s[1], $s[2], $s[3])
  Write-Host "INFO  Arabic: joined $jc shape(s) $($ji[1] - $ji[0] + 1) px wide, apart (ZWNJ) $sc shapes $($si[1] - $si[0] + 1) px wide"
  if (-not ($jc -ge 1 -and $jc -lt $sc -and ($ji[1] - $ji[0]) -lt ($si[1] - $si[0]))) { $out += "Arabic not joined: $jc shapes against $sc" }
  # Alignment: textDirection rtl (right), textAlignment viewStart (with the layout),
  # gravity end of an RTL paragraph (left).
  $a = $v['latin_rtl']; $ink = [GesherRtl]::InkColumns($px, $a[0], $a[1], $a[2], $a[3])
  if (-not (AlignedRight $ink $a)) { $out += "textDirection rtl: Latin not aligned right (ink $($ink -join '..'))" }
  $a = $v['view_start']; $ink = [GesherRtl]::InkColumns($px, $a[0], $a[1], $a[2], $a[3])
  if ($Rtl -and -not (AlignedRight $ink $a)) { $out += "textAlignment viewStart in an RTL layout: not right (ink $($ink -join '..'))" }
  if (-not $Rtl -and -not (AlignedLeft $ink $a)) { $out += "textAlignment viewStart in an LTR layout: not left (ink $($ink -join '..'))" }
  $a = $v['gravity_end']; $ink = [GesherRtl]::InkColumns($px, $a[0], $a[1], $a[2], $a[3])
  if (-not (AlignedLeft $ink $a)) { $out += "gravity end of an RTL paragraph: not left (ink $($ink -join '..'))" }
  Write-Host "INFO  alignment (ink columns): textDirection rtl $((& { $a = $v['latin_rtl']; [GesherRtl]::InkColumns($px, $a[0], $a[1], $a[2], $a[3]) }) -join '..'), viewStart $((& { $a = $v['view_start']; [GesherRtl]::InkColumns($px, $a[0], $a[1], $a[2], $a[3]) }) -join '..'), gravity end $($ink -join '..') of 400"
  # CJK glyphs, not the .notdef of a code point no font has; emoji in color.
  $c = $v['cjk']; $t = $v['tofu']; $e = $v['emoji']
  $cjkDark = [GesherRtl]::DarkCount($px, $c[0], $c[1], $c[2], $c[3])
  $tofuDark = [GesherRtl]::DarkCount($px, $t[0], $t[1], $t[2], $t[3])
  $emojiColor = [GesherRtl]::Colorful($px, $e[0], $e[1], $e[2], $e[3])
  Write-Host "INFO  CJK: $cjkDark dark pixels (tofu: $tofuDark); emoji: $emojiColor colored pixels"
  if ($cjkDark -lt 400 -or $cjkDark -lt $tofuDark * 1.3) { $out += "CJK looks like tofu ($cjkDark dark pixels, tofu $tofuDark)" }
  if ($emojiColor -lt 200) { $out += "emoji not in color ($emojiColor colored pixels)" }
  if ($Text -notmatch 'RtlActivity glyphs: cjk true/true/true, emoji true, tofu false, arabic true') { $out += 'Paint.hasGlyph: not every script has glyphs' }
  return $out
}

# Posts a key to the window as a keyboard does (WM_KEYDOWN/UP; arrows, Home and End
# are extended keys).
function RtlKey([IntPtr] $Hwnd, [int] $Vk, [bool] $Down) {
  $scan = [GesherWin32]::MapVirtualKeyW([uint32]$Vk, 0)
  $l = 1 -bor ([long]$scan -shl 16)
  if ($Vk -in 0x23, 0x24, 0x25, 0x26, 0x27, 0x28) { $l = $l -bor (1L -shl 24) }
  if (-not $Down) { $l = $l -bor (3L -shl 30) }
  [void][GesherWin32]::PostMessageW($Hwnd, $(if ($Down) { 0x0100 } else { 0x0101 }), [IntPtr]$Vk, [IntPtr]$l)
}
function RtlPress([IntPtr] $Hwnd, [int] $Vk, [switch] $Shift) {
  if ($Shift) { RtlKey $Hwnd 0x10 $true }
  RtlKey $Hwnd $Vk $true
  RtlKey $Hwnd $Vk $false
  if ($Shift) { RtlKey $Hwnd 0x10 $false }
}

# The cursor in the RTL EditText (focused, at offset 0: its right end) moved by
# arrow keys as it looks: left goes forward in the Hebrew text, right back,
# shift+left selects; End and Home go to the right and left edges, as Android's
# ArrowKeyMovementMethod moves them (Selection.moveToRightEdge/LeftEdge): the
# logical start and end of RTL text.
function RtlAppCursor([IntPtr] $Hwnd, [scriptblock] $ReadLog) {
  $out = @()
  if ("$(& $ReadLog)" -notmatch 'RtlActivity edit: paragraph rtl, cursor x at 0: ([\d.]+), at 4: ([\d.]+), focused true') {
    $out += 'the EditText is not a focused RTL paragraph'
  } elseif ([double]$Matches[1] -le [double]$Matches[2]) {
    $out += "the cursor at offset 0 (x $($Matches[1])) is not right of the end (x $($Matches[2]))"
  }
  $steps = @(@(0x25, $false, 'RtlActivity selection 1-1'), @(0x25, $false, 'RtlActivity selection 2-2'),
             @(0x27, $false, 'RtlActivity selection 1-1'), @(0x25, $true, 'RtlActivity selection 1-2'),
             @(0x23, $false, 'RtlActivity selection 0-0'), @(0x24, $false, 'RtlActivity selection 4-4'))
  $seen = 0
  foreach ($s in $steps) {
    $before = ([regex]::Matches("$(& $ReadLog)", [regex]::Escape($s[2]))).Count
    RtlPress $Hwnd $s[0] -Shift:$s[1]
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and ([regex]::Matches("$(& $ReadLog)", [regex]::Escape($s[2]))).Count -le $before) { Start-Sleep -Milliseconds 200 }
    if (([regex]::Matches("$(& $ReadLog)", [regex]::Escape($s[2]))).Count -le $before) {
      $key = @{ 0x25 = 'Left'; 0x27 = 'Right'; 0x23 = 'End'; 0x24 = 'Home' }[$s[0]]
      $out += "after $(if ($s[1]) { 'shift+' })$key no '$($s[2])'"
      break
    }
    $seen++
  }
  Write-Host "INFO  RTL EditText: $seen of $($steps.Count) cursor moves as expected"
  return $out
}

if (-not (Get-Command WindowAppCheck -ErrorAction SilentlyContinue)) {
  Write-Host 'FAIL  rtl: WindowAppCheck (34-windows.ps1) is not defined'
  $failed++
} else {
  $rtlExpect = @('RtlActivity onResume', 'window "Gesher RTL test" added', 'closed by the user', 'RtlActivity onDestroy', 'gesher-app: done')
  $showLog = { param($text) ("$text" -split "`n" | Select-String -Pattern '^RtlActivity (locales|formats|glyphs|edit)') | Out-Host }

  # a. The runner's locale (en-US): left to right layout; RTL paragraphs still
  #    right to left.
  WindowAppCheck 'rtlapp.apk under a left-to-right locale: Hebrew and Arabic TextViews drawn right to left and aligned right, mixed bidi runs in order, Arabic joined, textDirection/textAlignment/gravity, CJK and color emoji glyphs (single process)' `
    rtlapp.apk @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/rtlapp.apk")) `
    { param($px, $hwnd, $text, $readLog) & $showLog $text; @(RtlAppPixels $px $text $false 'ltr') } `
    $rtlExpect

  # b. $GESHER_LOCALES=he-IL: the layout mirrored, Hebrew resources (values-iw), the
  #    cursor moved by arrow keys.
  $env:GESHER_LOCALES = 'he-IL,en-US'
  try {
    WindowAppCheck 'rtlapp.apk under he-IL: the layout mirrored (layoutDirection from the locale, supportsRtl), Hebrew resources, textAlignment viewStart on the right; arrow keys move the cursor through the RTL EditText as it looks (single process)' `
      rtlapp.apk @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160', (NoDrive "$Dir/rtlapp.apk")) `
      { param($px, $hwnd, $text, $readLog) & $showLog $text; @(RtlAppPixels $px $text $true 'rtl-he') + @(RtlAppCursor $hwnd $readLog) } `
      ($rtlExpect + 'RtlActivity locales he-IL,en-US')
  } finally {
    Remove-Item Env:GESHER_LOCALES -ErrorAction SilentlyContinue
  }

  # c. The Gesher device under ar-EG: the system server's configuration (its
  #    locales from $GESHER_LOCALES) reaches the app process it starts.
  $savedData = $env:ANDROID_DATA
  $savedLogs = $env:GESHER_LOGS
  $env:ANDROID_DATA = NoDrive "$root/data-rtl"
  $env:GESHER_LOGS = NoDrive "$root/logs-rtl"
  New-Item -ItemType Directory -Force "$root/data-rtl" | Out-Null
  $env:GESHER_SHORTCUTS = '0'
  $env:GESHER_LOCALES = 'ar-EG'
  try {
    WindowAppCheck 'the Gesher device under ar-EG: rtlapp.apk''s app process gets the system server''s right-to-left configuration (mirrored layout, Arabic resources), its Hebrew and Arabic text drawn right to left' `
      rtlapp.apk @('com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/rtlapp.apk"), '--hidden-window') `
      { param($px, $hwnd, $text, $readLog) & $showLog $text; @(RtlAppPixels $px $text $true 'rtl-ar') } `
      @('gesher-app: app process for com.gesher.rtlapp: pid ', 'RtlActivity locales ar-EG', 'RtlActivity onResume',
        'closed by the user', 'RtlActivity onDestroy', 'gesher-app: the process of com.gesher.rtlapp ended (exit code 0)',
        'gesher-app: done')
  } finally {
    Remove-Item Env:GESHER_LOCALES -ErrorAction SilentlyContinue
    $shutdownLog = Join-Path $env:RUNNER_TEMP 'rtl-device-shutdown.log'
    $sd = Start-Process "$Dir/dalvikvm.exe" -ArgumentList (@($dvmWindows) + 'com.gesher.app.Launcher', 'shutdown') `
      -NoNewWindow -PassThru -RedirectStandardOutput $shutdownLog -RedirectStandardError "$shutdownLog.err"
    if (-not $sd.WaitForExit(120000)) { $sd.Kill($true); Write-Host 'INFO  the rtl device shutdown timed out' }
    if ($failed) {
      foreach ($f in Get-ChildItem "$root/logs-rtl" -Filter *.log -ErrorAction SilentlyContinue) {
        Write-Host "--- $($f.Name)"
        Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -Last 60 | Out-Host
      }
    }
    $env:ANDROID_DATA = $savedData
    if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
    Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
  }
}
if ($null -ne $savedLocales) { $env:GESHER_LOCALES = $savedLocales }
