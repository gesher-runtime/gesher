# Builds ANGLE's libEGL.dll and libGLESv2.dll (Direct3D 11 backend, x64) from
# source with ANGLE's own GN build: depot_tools at a pinned commit, a shallow
# gclient sync of ANGLE's DEPS without what the two DLLs do not need, Chromium's
# clang (clang-cl/lld-link) against the runner's Visual Studio 2022 and Windows
# SDK (DEPOT_TOOLS_WIN_TOOLCHAIN=0). Run by .github/workflows/angle.yml on a
# Windows runner; see docs/angle.md.
#
#   pwsh compat/angle/build-angle.ps1 -Commit <angle sha> -DepotTools <sha> -Work C:\angle -Out <dir>
#
# <Out> gets bin\ (the DLLs), lib\ (their import libraries), include\ (EGL,
# GLES, KHR headers), NOTICES.txt (the licenses of what the DLLs contain) and
# ANGLE_COMMIT.txt.
param(
  [Parameter(Mandatory)] [string] $Commit,
  [Parameter(Mandatory)] [string] $DepotTools,
  [Parameter(Mandatory)] [string] $Work,
  [Parameter(Mandatory)] [string] $Out
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$googlesource = 'https://chromium.googlesource.com'

# googlesource answers bursts with HTTP 429: retry with growing pauses.
function Retry([scriptblock] $Block, [int] $Tries = 5) {
  for ($i = 1; ; $i++) {
    try { & $Block; return } catch {
      if ($i -ge $Tries) { throw }
      Write-Host "::warning::attempt $i failed ($_); retrying in $(60 * $i) s"
      Start-Sleep (60 * $i)
    }
  }
}

function Step([string] $Name) {
  Write-Host "::endgroup::"
  Write-Host "::group::$Name ($(Get-Date -Format HH:mm:ss))"
}

# A shallow checkout of one commit.
function Fetch([string] $Url, [string] $Sha, [string] $Dir) {
  if (-not (Test-Path "$Dir/.git")) {
    git init -q $Dir
    git -C $Dir remote add origin $Url
  }
  Retry { git -C $Dir fetch -q --depth 1 origin $Sha }
  git -C $Dir checkout -q --force FETCH_HEAD
}

Step 'Machine'
Get-PSDrive -PSProvider FileSystem | Format-Table Name, @{n='FreeGB'; e={[int]($_.Free / 1GB)}} | Out-Host
git config --global core.autocrlf false
git config --global core.longpaths true
New-Item -ItemType Directory -Force $Work | Out-Null

Step "depot_tools at $DepotTools"
$dt = Join-Path $Work 'depot_tools'
Fetch "$googlesource/chromium/tools/depot_tools.git" $DepotTools $dt
$env:DEPOT_TOOLS_UPDATE = '0'          # stay at the pinned commit
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'   # the runner's Visual Studio, not Google's package
$env:DEPOT_TOOLS_METRICS = '0'
$env:GCLIENT_SUPPRESS_GIT_VERSION_WARNING = '1'
$env:PATH = "$dt;$env:PATH"
# Installs depot_tools' Python (CIPD) and its python3.bat/git wrappers.
cmd /d /c "`"$dt\bootstrap\win_tools.bat`""
if ($LASTEXITCODE -ne 0) { throw "depot_tools bootstrap failed ($LASTEXITCODE)" }

Step "ANGLE at $Commit"
$src = Join-Path $Work 'angle'
Fetch "$googlesource/angle/angle.git" $Commit $src

# The checkout is the (unmanaged) solution, as ANGLE's scripts/bootstrap.py
# sets it up. custom_deps drops what libEGL/libGLESv2 with only the D3D11
# backend never reach: test suites and their data, other backends (Vulkan,
# SwiftShader, Dawn, OpenCL), LLVM, Rust crates, Android/perf tooling.
$skip = @(
  'third_party/llvm/src', 'third_party/VK-GL-CTS/src', 'third_party/dawn',
  'third_party/SwiftShader', 'third_party/catapult', 'third_party/OpenCL-CTS/src',
  'third_party/OpenCL-Docs/src', 'third_party/OpenCL-ICD-Loader/src',
  'third_party/vulkan-validation-layers/src', 'third_party/lunarg-vulkantools/src',
  'third_party/vulkan-tools/src', 'third_party/vulkan-loader/src',
  'third_party/clspv/src', 'third_party/glmark2/src', 'third_party/cherry',
  'third_party/rust', 'third_party/protobuf', 'third_party/libjpeg_turbo',
  'third_party/libpng/src', 'third_party/nasm', 'third_party/libdrm/src',
  'third_party/OpenGL-Registry/src', 'third_party/EGL-Registry/src',
  'third_party/clang-format/script', 'third_party/siso/cipd',
  'third_party/Python-Markdown', 'buildtools/reclient', 'tools/perf',
  'tools/md_browser', 'tools/memory', 'tools/mb', 'tools/valgrind',
  'tools/skia_goldctl/win'
)
$customDeps = ($skip | ForEach-Object { "      `"$_`": None," }) -join "`n"
@"
solutions = [
  {
    "name": ".",
    "url": "$googlesource/angle/angle.git",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {
$customDeps
    },
    "custom_vars": {},
  },
]
"@ | Set-Content -Encoding ascii (Join-Path $src '.gclient')

Push-Location $src
try {
  Step 'gclient sync (shallow, no hooks)'
  Retry { gclient sync --nohooks --no-history --shallow --jobs 8 }
  if (-not (Test-Path 'build/config/gclient_args.gni')) { throw 'gclient did not write build/config/gclient_args.gni' }
  Get-ChildItem third_party, build, buildtools, tools -Directory | ForEach-Object {
    $mb = [int]((Get-ChildItem $_.FullName -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB)
    if ($mb -ge 20) { '{0,6} MB  {1}' -f $mb, (Resolve-Path -Relative $_.FullName) }
  } | Out-Host

  # The hooks the build needs, from DEPS (the rest download test tools,
  # sysroots, Rust and Google's Visual Studio package).
  Step 'Hooks: clang, LASTCHANGE, rc.exe'
  Retry { python3 tools/clang/scripts/update.py }
  python3 build/util/lastchange.py -o build/util/LASTCHANGE
  # rc_win: Chromium's rc.exe from the chromium-browser-clang bucket, by hash.
  $rcSha = (Get-Content build/toolchain/win/rc/win/rc.exe.sha1 -Raw).Trim()
  Retry { Invoke-WebRequest "https://storage.googleapis.com/chromium-browser-clang/rc/$rcSha" -OutFile build/toolchain/win/rc/win/rc.exe }
  $got = (Get-FileHash -Algorithm SHA1 build/toolchain/win/rc/win/rc.exe).Hash.ToLower()
  if ($got -ne $rcSha) { throw "rc.exe: sha1 $got, expected $rcSha" }

  Step 'gn gen'
  New-Item -ItemType Directory -Force out/Release | Out-Null
  Copy-Item (Join-Path $PSScriptRoot 'args.gn') out/Release/args.gn
  & buildtools/win/gn.exe gen out/Release
  & buildtools/win/gn.exe args out/Release --list --short --overrides-only | Out-Host

  Step 'ninja libEGL libGLESv2'
  $t = Get-Date
  & third_party/ninja/ninja.exe -C out/Release libEGL libGLESv2
  Write-Host "built in $([int]((Get-Date) - $t).TotalMinutes) min"

  Step "Collect into $Out"
  foreach ($d in 'bin', 'lib', 'include') { New-Item -ItemType Directory -Force (Join-Path $Out $d) | Out-Null }
  foreach ($n in 'libEGL', 'libGLESv2') {
    Copy-Item "out/Release/$n.dll" (Join-Path $Out 'bin')
    Copy-Item "out/Release/$n.dll.lib" (Join-Path $Out 'lib')
  }
  foreach ($d in 'EGL', 'GLES', 'GLES2', 'GLES3', 'KHR') {
    Copy-Item -Recurse "include/$d" (Join-Path $Out 'include')
  }
  $Commit | Set-Content (Join-Path $Out 'ANGLE_COMMIT.txt')

  # NOTICES.txt: ANGLE's BSD license, then every third-party license of code
  # linked into the DLLs (ANGLE's own src/**/third_party, zlib, abseil,
  # Chromium's libc++/libc++abi, which the DLLs link statically).
  $licenses = @('LICENSE') +
    @(Get-ChildItem src -Recurse -File -Include 'LICENSE*', 'COPYING*' |
      Where-Object { $_.FullName -match '[\\/]third_party[\\/]' } |
      ForEach-Object { Resolve-Path -Relative $_.FullName }) +
    @('third_party/zlib/LICENSE', 'third_party/abseil-cpp/LICENSE',
      'third_party/libc++/src/LICENSE.TXT', 'third_party/libc++abi/src/LICENSE.TXT')
  $notices = foreach ($l in $licenses) {
    if (-not (Test-Path $l)) { Write-Host "::warning::no $l"; continue }
    '=' * 78
    "ANGLE $Commit -- $(($l -replace '\\', '/') -replace '^\./', '')"
    '=' * 78
    Get-Content $l
    ''
  }
  @("libEGL.dll and libGLESv2.dll are ANGLE (https://chromium.googlesource.com/angle/angle),",
    "built from commit $Commit. ANGLE is under the BSD license below; the",
    "third-party code linked into the DLLs follows with its own licenses.", '') +
    $notices | Set-Content -Encoding utf8 (Join-Path $Out 'NOTICES.txt')
  Get-ChildItem -Recurse $Out -File | Format-Table Length, FullName | Out-Host
} finally {
  Pop-Location
  Write-Host "::endgroup::"
}
