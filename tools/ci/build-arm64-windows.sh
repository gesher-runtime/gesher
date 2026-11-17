#!/usr/bin/env bash
# Shared by the hosted baseline and the self-hosted PR trial.
set -euo pipefail

build_args=()
if [[ -n "${BUILD_JOBS:-}" ]]; then
  [[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid BUILD_JOBS" >&2; exit 1; }
  build_args=(-j "$BUILD_JOBS")
fi

ccache --set-config=max_size=2G
cmake -S . -B out-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw-arm64.cmake \
  -DLLVM_MINGW="$HOME/toolchain/$LLVM_MINGW_NAME" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

ninja -C out-arm64 "${build_args[@]}" \
  platform_probe \
  socket_probe \
  mman_probe \
  madvise_probe \
  epoll_probe \
  posix_probe \
  timerfd_windows_test \
  boottime_resume_windows_test \
  gesher_elf \
  gesher_bionic \
  gesher_posix \
  arm64_elf_test

OBJDUMP="$HOME/toolchain/$LLVM_MINGW_NAME/bin/llvm-objdump"
binaries=(
  out-arm64/platform_probe.exe
  out-arm64/socket_probe.exe
  out-arm64/mman_probe.exe
  out-arm64/madvise_probe.exe
  out-arm64/epoll_probe.exe
  out-arm64/posix_probe.exe
  out-arm64/timerfd_windows_test.exe
  out-arm64/boottime_resume_windows_test.exe
  out-arm64/arm64_elf_test.exe
)
for b in "${binaries[@]}"; do
  echo "Checking architecture for $b..."
  info=$("$OBJDUMP" -f "$b")
  echo "$info"
  echo "$info" | grep -iE 'aarch64|arm64' || {
    echo "ERROR: $b is not aarch64!"
    exit 1
  }
done
echo "All binaries verified as aarch64!"
