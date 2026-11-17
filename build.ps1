# Basic PowerShell Build Script for Gesher
$buildDir = "build_output"
if (!(Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir
}
Set-Location $buildDir

# Configure with CMake
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-llvm-mingw.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed"
    exit 1
}

# Build
cmake --build . --config RelWithDebInfo
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

Write-Output "Build completed successfully."
