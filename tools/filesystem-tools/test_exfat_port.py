#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Validate the external exFAT port with isolated host boundaries and real files.

No test opens a physical disk or grants device authority. The block cases model
fstat/ioctl at an explicit boundary; attribute openat/read/close operate on real
temporary files. A second executable runs the actual upstream device-info
function on regular image files, with the unported source as a negative control.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import types


HERE = Path(__file__).resolve().parent

PROBE_FIXTURE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "fixture invariant failed at line %d: %s\n", __LINE__, #condition); \
  exit(90); } } while (0)

/* Only this isolated executable substitutes the host block boundary. */
static int target, block_mode, auxiliary, probes, target_stats, reads, opens;
static unsigned long long supplied_bytes;
static int supplied_sector;
static const char *directory, *fault;

static int fixture_fstat(int fd, struct stat *status)
{
  if (fd == target) {
    ++target_stats;
    if (!strcmp(fault, "fstat-target")) { errno = EACCES; return -1; }
  } else if (!strcmp(fault, "fstat-directory")) { errno = EACCES; return -1; }
  int result = fstat(fd, status);
  if (result == 0 && fd == target && block_mode) {
    status->st_mode = (status->st_mode & 0777) | (block_mode == 1 ? S_IFBLK : S_IFCHR);
    status->st_rdev = makedev(8, 17);
    if (!strcmp(fault, "identity-change") && target_stats >= 2)
      status->st_rdev = makedev(8, 18);
  }
  return result;
}

static int fixture_ioctl(int fd, unsigned long request, ...)
{
  CHECK(fd == target);
  ++probes;
  if (!strcmp(fault, "ioctl-denied")) { errno = EACCES; return -1; }
  if (!strcmp(fault, "ioctl-revoked") && probes >= 3) { errno = ENODEV; return -1; }
  if (!strcmp(fault, "sector-unsupported") && request == BLKSSZGET) {
    errno = ENOTTY; return -1;
  }
  va_list args;
  va_start(args, request);
  void *output = va_arg(args, void *);
  va_end(args);
  if (request == BLKGETSIZE64) {
    *(uint64_t *)output = supplied_bytes;
    if (!strcmp(fault, "size-change") && probes >= 3)
      *(uint64_t *)output += 512;
  } else if (request == BLKSSZGET) {
    *(int *)output = supplied_sector;
    if (!strcmp(fault, "sector-change") && probes >= 4)
      *(int *)output *= 2;
  } else { errno = ENOTTY; return -1; }
  return 0;
}

static int fixture_open(const char *path, int flags, ...)
{
  ++opens;
  CHECK(!strcmp(path, "/sys/dev/block/8:17/"));
  CHECK((flags & O_ACCMODE) == O_RDONLY && !(flags & (O_TRUNC | O_CREAT)));
  CHECK(flags & O_DIRECTORY);
  CHECK(flags & O_CLOEXEC);
  if (!strcmp(fault, "directory-denied")) { errno = EACCES; return -1; }
  if (!strcmp(fault, "directory-missing")) { errno = ENOENT; return -1; }
  int fd = open(directory, flags);
  if (fd >= 0) ++auxiliary;
  return fd;
}

static int fixture_openat(int fd, const char *name, int flags, ...)
{
  CHECK(fd != target);
  CHECK((flags & O_ACCMODE) == O_RDONLY && !(flags & (O_TRUNC | O_CREAT)));
  CHECK(flags & O_CLOEXEC);
  CHECK(!strcmp(name, "size") || !strcmp(name, "partition") ||
        !strcmp(name, "start") || !strcmp(name, "../size"));
  if (!strncmp(fault, "deny:", 5) && !strcmp(name, fault + 5)) {
    errno = EACCES; return -1;
  }
  int result = openat(fd, name, flags);
  if (result >= 0) ++auxiliary;
  return result;
}

static ssize_t fixture_read(int fd, void *buffer, size_t length)
{
  CHECK(fd != target);
  ++reads;
  if (!strcmp(fault, "read-failed")) { errno = EIO; return -1; }
  if (!strcmp(fault, "read-interrupted") && reads == 1) { errno = EINTR; return -1; }
  return read(fd, buffer, length);
}

static int fixture_close(int fd)
{
  CHECK(fd != target && auxiliary > 0);
  --auxiliary;
  int result = close(fd);
  if (!strcmp(fault, "close-failed")) { errno = EIO; return -1; }
  return result;
}

#define fstat fixture_fstat
#define ioctl fixture_ioctl
#define open fixture_open
#define openat fixture_openat
#define read fixture_read
#define close fixture_close
#include "exfat_capabilities.h"
#undef fstat
#undef ioctl
#undef open
#undef openat
#undef read
#undef close

int main(int argc, char **argv)
{
  CHECK(argc == 7);
  directory = argv[1];
  target = open(argv[2], O_RDONLY | O_CLOEXEC);
  CHECK(target >= 0);
  block_mode = atoi(argv[3]);
  supplied_bytes = strtoull(argv[4], NULL, 10);
  supplied_sector = (int)strtol(argv[5], NULL, 10);
  fault = argv[6];
  unsigned char before[32], after[32];
  CHECK(pread(target, before, sizeof(before), 0) == sizeof(before));
  CHECK(lseek(target, 37, SEEK_SET) == 37);
  struct gesher_exfat_geometry geometry, untouched;
  memset(&geometry, 0xa5, sizeof(geometry));
  memcpy(&untouched, &geometry, sizeof(untouched));
  char path[PATH_MAX];
  size_t capacity = !strcmp(fault, "short-path") ? 8 : sizeof(path);
  errno = 0;
  int result = gesher_exfat_check_target(target, path, capacity, &geometry);
  int error = errno;
  CHECK(auxiliary == 0);
  CHECK(lseek(target, 0, SEEK_CUR) == 37);
  CHECK(pread(target, after, sizeof(after), 0) == sizeof(after));
  CHECK(!memcmp(before, after, sizeof(before)));
  if (result < 0) CHECK(!memcmp(&geometry, &untouched, sizeof(geometry)));
  printf("%d %d %d %llu %llu %u %u %d %d\n", result, error,
         result == 0 ? geometry.is_block : 0,
         result == 0 ? (unsigned long long)geometry.bytes : 0,
         result == 0 ? (unsigned long long)geometry.offset_bytes : 0,
         result == 0 ? geometry.sector_bytes : 0,
         result == 0 ? geometry.partition : 0, probes, opens);
  CHECK(close(target) == 0);
  return 0;
}
'''


UPSTREAM_REGULAR_FIXTURE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "exfat_ondisk.h"
#include "libexfat.h"

static int release_queries;
/* Model the actual Gesher release in this host fixture. The port must not call
 * this boundary at all. The unported upstream negative control must reject it.
 */
int uname(struct utsname *result)
{
  ++release_queries;
  memset(result, 0, sizeof(*result));
  strcpy(result->release, "0.0.0-gesher");
  return 0;
}

int main(int argc, char **argv)
{
  if (argc != 5) return 80;
  struct exfat_user_input input;
  struct exfat_blk_dev device;
  exfat_init_user_input(&input);
  exfat_init_blk_dev_info(&device);
  input.dev_name = argv[1];
  input.writeable = atoi(argv[2]) != 0;
  input.sector_size = (unsigned int)atoi(argv[3]);
  input.cluster_size = 4096;
  int negative = atoi(argv[4]);
  int result = exfat_get_blk_dev_info(&input, &device);
  if (negative == 1) {
    if (result != -1 || release_queries != 1 || device.dev_fd != -1) return 81;
    puts("UNPORTED_RELEASE_GATE_REJECTED");
  } else if (negative == 2) {
    if (result != -1 || release_queries != 0 || errno != ENOENT) return 82;
    puts("PORTED_REAL_OPEN_FAILURE_PRESERVED");
  } else {
    unsigned int expected = input.sector_size ? input.sector_size : 512;
    if (result != 0 || release_queries != 0 || device.dev_fd < 0 ||
        device.size != 8388608 || device.offset != 0 || device.isblk || device.ispart ||
        device.sector_size != expected || device.num_sectors != 8388608 / expected)
      return 83;
    puts("PORTED_UPSTREAM_REGULAR_FILE_PASSED");
    exfat_deinit_blk_dev_info(&device);
  }
  return 0;
}
'''


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def rejected(callback, label: str) -> None:
    try:
        callback()
    except RuntimeError:
        return
    raise RuntimeError(f"Negative integrity control did not fail: {label}")


def run(command: list[str]) -> str:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(result.returncode == 0, f"Command failed ({result.returncode}): {command}\n{result.stdout}")
    return result.stdout


def capabilities(binary: Path, work: Path) -> int:
    checks = 0
    size = 64 * 1024 * 1024

    def case(name: str, *, expected: int = 0, mode: int = 1, sector: int = 512,
             byte_count: int = size, attributes: dict[str, str | bytes | None] | None = None,
             partition: bool = False, fault: str = "", offset: int = 0) -> None:
        nonlocal checks
        root = work / name
        root.mkdir()
        disk = root / "disk"
        disk.mkdir()
        directory = disk
        fields: dict[str, str | bytes | None] = {"size": str(size // 512) + "\n"}
        if partition:
            directory = disk / "partition"
            directory.mkdir()
            fields.update({"partition": "1\n", "start": "2048\n", "../size": "133120\n"})
        fields.update(attributes or {})
        for attribute, contents in fields.items():
            if contents is not None:
                (directory / attribute).write_bytes(contents if isinstance(contents, bytes) else contents.encode())
        target = root / "image"
        target.write_bytes(b"Z" * 65536)
        before = hashlib.sha256(target.read_bytes()).hexdigest()
        output = run([str(binary), str(directory), str(target), str(mode), str(byte_count),
                      str(sector), fault])
        values = [int(value) for value in output.strip().split()]
        require(len(values) == 9, f"Bad capability fixture result: {output}")
        result, error, is_block, actual_bytes, actual_offset, actual_sector, part, probes, opens = values
        require(result == (-1 if expected else 0) and (not expected or error == expected),
                f"{name}: expected errno={expected}; got {output.strip()}")
        require(hashlib.sha256(target.read_bytes()).hexdigest() == before, f"{name}: target bytes changed")
        if not expected and mode == 0:
            require(not any((is_block, actual_bytes, actual_offset, actual_sector, part, probes, opens)),
                    f"{name}: regular image acquired block requirements")
        elif not expected:
            require((is_block, actual_bytes, actual_offset, actual_sector, part) ==
                    (1, byte_count, offset, sector, 1 if partition else 0), f"{name}: incorrect geometry")
            require(probes == 4 and opens == 1, f"{name}: required exact-object probes missing")
        checks += 1

    case("regular-file", mode=0, fault="directory-denied")
    case("whole-device")
    case("partition", partition=True, offset=1048576)
    case("sector-4096", partition=True, sector=4096, offset=1048576)
    case("read-interrupted", fault="read-interrupted")
    case("trailing-whitespace", attributes={"size": "131072 \r\n\t"})
    case("character-device", mode=2, expected=errno.ENOTSUP)
    case("target-stat-denied", fault="fstat-target", expected=errno.EACCES)
    case("directory-stat-denied", fault="fstat-directory", expected=errno.EACCES)
    case("directory-denied", fault="directory-denied", expected=errno.EACCES)
    case("directory-missing", fault="directory-missing", expected=errno.ENOENT)
    case("ioctl-denied", fault="ioctl-denied", expected=errno.EACCES)
    case("sector-unsupported", fault="sector-unsupported", expected=errno.ENOTTY)
    case("revoked-after-metadata", fault="ioctl-revoked", expected=errno.ENODEV)
    case("changed-identity", fault="identity-change", expected=errno.ESTALE)
    case("changed-size", fault="size-change", expected=errno.ESTALE)
    case("changed-sector", fault="sector-change", expected=errno.ESTALE)
    case("short-path", fault="short-path", expected=errno.ENAMETOOLONG)
    case("read-failed", fault="read-failed", expected=errno.EIO)
    case("close-failed", fault="close-failed", expected=errno.EIO)
    case("zero-size", byte_count=0, expected=errno.EIO)
    case("size-too-large", byte_count=2**63, expected=errno.EIO)
    case("unaligned-size", byte_count=size + 1, expected=errno.EIO)
    for value in (0, -512, 256, 513, 768):
        case(f"invalid-sector-{value}", sector=value, expected=errno.EIO)
    case("size-missing", attributes={"size": None}, expected=errno.ENOENT)
    case("size-denied", fault="deny:size", expected=errno.EACCES)
    case("size-disagrees", attributes={"size": "131071\n"}, expected=errno.EIO)
    for name, value, error in (
        ("empty", "", errno.EIO), ("negative", "-1\n", errno.EIO),
        ("sign", "+131072\n", errno.EIO), ("leading-space", " 131072\n", errno.EIO),
        ("suffix", "131072x", errno.EIO), ("embedded-nul", b"131072\0\n", errno.EIO),
        ("overflow", "18446744073709551616\n", errno.EOVERFLOW),
        ("overlong", "1" * 64, errno.EOVERFLOW),
    ):
        case(f"size-{name}", attributes={"size": value}, expected=error)
    case("partition-denied", fault="deny:partition", expected=errno.EACCES)
    case("start-without-partition", attributes={"start": "2048\n"}, expected=errno.EIO)
    case("whole-start-denied", fault="deny:start", expected=errno.EACCES)
    case("zero-partition", partition=True, attributes={"partition": "0"}, expected=errno.EIO)
    case("large-partition", partition=True, attributes={"partition": "4294967296"}, expected=errno.EIO)
    case("partition-start-missing", partition=True, attributes={"start": None}, expected=errno.ENOENT)
    case("partition-start-denied", partition=True, fault="deny:start", expected=errno.EACCES)
    case("partition-parent-missing", partition=True, attributes={"../size": None}, expected=errno.ENOENT)
    case("partition-parent-denied", partition=True, fault="deny:../size", expected=errno.EACCES)
    case("partition-parent-size-overflow", partition=True,
         attributes={"../size": "18014398509481984"}, expected=errno.EIO)
    case("partition-parent-zero", partition=True, attributes={"../size": "0"}, expected=errno.EIO)
    case("partition-outside-parent", partition=True, attributes={"../size": "133119"}, expected=errno.EIO)
    case("partition-start-after-parent", partition=True, attributes={"start": "133121"}, expected=errno.EIO)
    case("partition-offset-overflow", partition=True,
         attributes={"start": "36028797018963968", "../size": "18446744073709551615"},
         expected=errno.EIO)
    case("partition-sector-misaligned", partition=True, sector=4096,
         attributes={"start": "2049", "../size": "133121"}, expected=errno.EIO)
    return checks


def upstream_regular(compiler: str, flags: list[str], work: Path,
                     original: Path, modified: Path) -> int:
    fixture = work / "regular-info.c"
    fixture.write_text(UPSTREAM_REGULAR_FIXTURE)
    image = work / "regular.img"
    with image.open("wb") as stream:
        stream.truncate(8388608)
    original_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    programs = {}
    for label, source in (("unported", original), ("ported", modified)):
        include = ["-I" + str(source / "include")]
        # Compile unmodified upstream units without imposing Gesher warning
        # policy on unrelated upstream code; our fixture/helper use -Werror.
        objects = []
        for name in ("libexfat", "utils"):
            obj = work / f"{label}-{name}.o"
            run([compiler, *flags, *include, "-c",
                 str(source / "lib" / (name + ".c")), "-o", str(obj)])
            objects.append(str(obj))
        main = work / f"{label}-main.o"
        run([compiler, *flags, "-Wall", "-Wextra", "-Werror", *include,
             "-c", str(fixture), "-o", str(main)])
        program = work / f"{label}-regular"
        run([compiler, *flags, "-Wl,--gc-sections", str(main), *objects, "-o", str(program)])
        programs[label] = program
    require("UNPORTED_RELEASE_GATE_REJECTED" in run(
        [str(programs["unported"]), str(image), "0", "0", "1"]), "Unported negative control missed gate")
    checks = 1
    for writable in (0, 1):
        for sector in (0, 4096):
            require("PORTED_UPSTREAM_REGULAR_FILE_PASSED" in run(
                [str(programs["ported"]), str(image), str(writable), str(sector), "0"]),
                "Actual upstream regular-file behavior failed")
            checks += 1
    require("PORTED_REAL_OPEN_FAILURE_PRESERVED" in run(
        [str(programs["ported"]), str(work / "absent.img"), "0", "0", "2"]),
        "Real absent-file error was lost")
    require(hashlib.sha256(image.read_bytes()).hexdigest() == original_hash,
            "Device-info tests changed regular-file contents")
    return checks + 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    work = args.work.resolve()
    checkout = HERE.parent.parent if (HERE.parent.parent / "AGENTS.md").is_file() else HERE.parent
    require(not work.is_relative_to(checkout), "Upstream test sources must stay outside the checkout/bundle")
    require(not work.exists(), "Port test work directory already exists; choose a fresh path")
    work.mkdir(parents=True)
    manifest = json.loads((HERE / "sources.json").read_text())
    spec, = manifest["ports"]
    package, = [item for item in manifest["packages"] if item["name"] == spec["package"]]
    recipe = (HERE / "port_exfat.py").read_bytes()
    require(hashlib.sha256(recipe).hexdigest() == spec["files"]["port_exfat.py"],
            "Port recipe SHA256 mismatch before loading")
    # Execute precisely the authenticated bytes, without loading stale cached
    # bytecode or creating __pycache__ inside a packaged source inventory.
    port_exfat = types.ModuleType("checked_exfat_port")
    exec(compile(recipe, str(HERE / "port_exfat.py"), "exec"), port_exfat.__dict__)
    port_exfat.check_port_files(HERE, spec)
    original = port_exfat.original_from_archive(args.archive, package, spec)
    modified, audit = port_exfat.transform(original, spec)
    require(len(audit["edits"]) == 7, "Unexpected scope of exFAT source edits")
    entry = b"int exfat_get_blk_dev_info("
    old_end = port_exfat.block_end(original, original.index(b"{", original.index(entry)))
    new_end = port_exfat.block_end(modified, modified.index(b"{", modified.index(entry)))
    require(original[old_end:] == modified[new_end:], "Port changed later upstream filesystem functions")
    rejected(lambda: port_exfat.transform(original + b"\n", spec), "changed original source")
    rejected(lambda: port_exfat.transform(modified, spec), "repeated source port")
    rejected(lambda: port_exfat.transform(original, dict(spec, output_sha256="0" * 64)),
             "changed expected transformed source")
    corrupt = work / "changed-port"
    corrupt.mkdir()
    for name in spec["files"]:
        shutil.copy2(HERE / name, corrupt / name)
    with (corrupt / "exfat_capabilities.h").open("ab") as output:
        output.write(b"\n")
    rejected(lambda: port_exfat.check_port_files(corrupt, spec), "changed helper source")
    shutil.copy2(HERE / "exfat_capabilities.h", corrupt / "exfat_capabilities.h")
    with (corrupt / "port_exfat.py").open("ab") as output:
        output.write(b"\n")
    rejected(lambda: port_exfat.check_port_files(corrupt, spec), "changed transform recipe")

    original_root = work / "original"
    original_root.mkdir()
    with tarfile.open(args.archive) as source:
        source.extractall(original_root, filter="data")
    source_original = original_root / package["directory"]
    source_modified = work / "ported"
    shutil.copytree(source_original, source_modified)
    require(port_exfat.apply(source_modified, spec, HERE) == audit, "Applied port differs from audited transform")

    flags = ["-std=gnu11", "-O1", "-g", "-D_FILE_OFFSET_BITS=64", "-ffunction-sections", "-fdata-sections"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    fixture = work / "capabilities.c"
    fixture.write_text(PROBE_FIXTURE)
    executable = work / "capabilities"
    run([args.cc, *flags, "-Wall", "-Wextra", "-Werror", "-I" + str(HERE), str(fixture), "-o", str(executable)])
    probes = capabilities(executable, work)
    regular = upstream_regular(args.cc, flags, work, source_original, source_modified)
    print(f"EXFAT_PORT_VALIDATED {probes} capability cases; {regular} actual upstream regular-file/negative cases; "
          "5 source-integrity negative controls; no physical device or Windows execution")
    print(f"EXFAT_PORT_TRANSFORM {audit['transformation_sha256']}")


if __name__ == "__main__":
    main()
