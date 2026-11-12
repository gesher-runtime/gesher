#!/usr/bin/env bash
# glibc 2.41 fixes the reference backend's sticky CLOEXEC and failed-freopen
# allocation bugs (32134 and 32140). Keep the real libc tests and all sanitizers.
# https://sourceware.org/pipermail/libc-announce/2025/000045.html
set -euo pipefail

readonly reference_image='docker.io/library/debian:trixie-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2'
repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly repository_root

printf 'Freopen reference image: %s\n' "$reference_image"
docker run --rm --interactive --pull=always --platform linux/amd64 \
  --mount "type=bind,source=${repository_root},target=/work,readonly" \
  --workdir /work \
  "$reference_image" /bin/bash -euo pipefail -s <<'FREOPEN_REFERENCE'
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
# Use the image's Debian archive configuration and signed package indexes.
apt-get update
apt-get install -y --no-install-recommends gcc g++ libc6-dev python3

reference_libc="$(getconf GNU_LIBC_VERSION)"
printf 'Freopen reference runtime: %s\n' "$reference_libc"
case "$reference_libc" in
  'glibc '*) ;;
  *) printf 'Expected a glibc reference runtime.\n' >&2; exit 1 ;;
esac
if ! dpkg --compare-versions "${reference_libc#glibc }" ge 2.41; then
  printf 'Freopen reference requires glibc 2.41 or newer.\n' >&2
  exit 1
fi

# The runner creates and removes its harness files under this container's /tmp.
# Run the existing ASan/UBSan suite with LSan enabled and no suppressions.
python3 /work/tools/test_freopen_mode_contracts.py --sanitize
FREOPEN_REFERENCE
