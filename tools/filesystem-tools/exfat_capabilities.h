/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * This independently written portability helper may be used under either
 * Apache License 2.0 (the bundled licenses/gesher-port/LICENSE) or MIT:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef GESHER_EXFAT_CAPABILITIES_H
#define GESHER_EXFAT_CAPABILITIES_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Queries concern the already-open target. This helper neither opens another
 * block object nor writes media. Regular file images retain upstream handling.
 */
struct gesher_exfat_geometry {
	int is_block;
	dev_t device;
	uint64_t bytes;
	uint64_t offset_bytes;
	unsigned int sector_bytes;
	uint32_t partition;
};

static int gesher_exfat_probe_error(int error)
{
	errno = error;
	return -1;
}

/* sysfs numbers are decimal and bounded. Distinguish an absent optional
 * attribute (ENOENT) from unreadable, empty, malformed or overflowing data.
 */
static int gesher_exfat_read_number(int directory, const char *name, uint64_t *value)
{
	char data[64];
	size_t length = 0, index = 0;
	uint64_t number = 0;
	int fd = openat(directory, name, O_RDONLY | O_CLOEXEC);
	int failure = 0;
	if (fd < 0)
		return -1;
	for (;;) {
		ssize_t count = read(fd, data + length, sizeof(data) - length);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			failure = errno;
			break;
		}
		if (count == 0)
			break;
		length += (size_t)count;
		if (length == sizeof(data)) {
			failure = EOVERFLOW;
			break;
		}
	}
	if (close(fd) != 0 && !failure)
		failure = errno;
	if (failure)
		return gesher_exfat_probe_error(failure);
	while (index < length && data[index] >= '0' && data[index] <= '9') {
		unsigned int digit = (unsigned int)(data[index++] - '0');
		if (number > (UINT64_MAX - digit) / 10)
			return gesher_exfat_probe_error(EOVERFLOW);
		number = number * 10 + digit;
	}
	if (index == 0)
		return gesher_exfat_probe_error(EIO);
	while (index < length && (data[index] == '\n' || data[index] == '\r' ||
			data[index] == ' ' || data[index] == '\t'))
		++index;
	if (index != length)
		return gesher_exfat_probe_error(EIO);
	*value = number;
	return 0;
}

static int gesher_exfat_recheck_target(int fd, const struct gesher_exfat_geometry *geometry)
{
	struct stat status;
	uint64_t bytes = 0;
	int sector = 0;
	if (!geometry->is_block)
		return 0;
	if (fstat(fd, &status) != 0)
		return -1;
	if (!S_ISBLK(status.st_mode) || status.st_rdev != geometry->device)
		return gesher_exfat_probe_error(ESTALE);
	if (ioctl(fd, BLKGETSIZE64, &bytes) != 0 || ioctl(fd, BLKSSZGET, &sector) != 0)
		return -1;
	if (bytes != geometry->bytes || sector <= 0 ||
			(unsigned int)sector != geometry->sector_bytes)
		return gesher_exfat_probe_error(ESTALE);
	return 0;
}

static int gesher_exfat_check_target(int fd, char *path, size_t capacity,
		struct gesher_exfat_geometry *result)
{
	struct stat status;
	struct gesher_exfat_geometry geometry = {0};
	uint64_t sectors = 0, partition = 0, start = 0, parent_sectors = 0;
	int sector = 0, directory = -1, failure = 0, length;
	if (fstat(fd, &status) != 0)
		return -1;
	if (S_ISREG(status.st_mode)) {
		*result = geometry;
		return 0;
	}
	if (!S_ISBLK(status.st_mode))
		return gesher_exfat_probe_error(ENOTSUP);
	geometry.is_block = 1;
	geometry.device = status.st_rdev;
	if (ioctl(fd, BLKGETSIZE64, &geometry.bytes) != 0 ||
			ioctl(fd, BLKSSZGET, &sector) != 0)
		return -1;
	if (!geometry.bytes || geometry.bytes > INT64_MAX || sector < 512 ||
			(sector & (sector - 1)) != 0 || geometry.bytes % (unsigned int)sector != 0)
		return gesher_exfat_probe_error(EIO);
	geometry.sector_bytes = (unsigned int)sector;
	length = snprintf(path, capacity, "/sys/dev/block/%u:%u/",
			major(status.st_rdev), minor(status.st_rdev));
	if (length < 0 || (size_t)length >= capacity)
		return gesher_exfat_probe_error(ENAMETOOLONG);
	directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0)
		return -1;
	if (fstat(directory, &status) != 0) {
		failure = errno;
		goto done;
	}
	if (!S_ISDIR(status.st_mode)) {
		failure = ENOTDIR;
		goto done;
	}
	if (gesher_exfat_read_number(directory, "size", &sectors) != 0) {
		failure = errno;
		goto done;
	}
	if (sectors != geometry.bytes / 512) {
		failure = EIO;
		goto done;
	}
	if (gesher_exfat_read_number(directory, "partition", &partition) != 0) {
		if (errno != ENOENT) {
			failure = errno;
			goto done;
		}
		/* A whole disk has neither partition nor start. A missing partition
		 * attribute must not reinterpret a known partition as a whole disk.
		 */
		if (gesher_exfat_read_number(directory, "start", &start) == 0)
			failure = EIO;
		else if (errno != ENOENT)
			failure = errno;
	} else {
		if (!partition || partition > UINT32_MAX) {
			failure = EIO;
			goto done;
		}
		if (gesher_exfat_read_number(directory, "start", &start) != 0 ||
				gesher_exfat_read_number(directory, "../size", &parent_sectors) != 0) {
			failure = errno;
			goto done;
		}
		if (!parent_sectors || parent_sectors > INT64_MAX / 512 || start > parent_sectors ||
				sectors > parent_sectors - start || (start * 512) % geometry.sector_bytes) {
			failure = EIO;
			goto done;
		}
		geometry.partition = (uint32_t)partition;
		geometry.offset_bytes = start * 512;
	}
done:
	if (close(directory) != 0 && !failure)
		failure = errno;
	if (failure)
		return gesher_exfat_probe_error(failure);
	if (gesher_exfat_recheck_target(fd, &geometry) != 0)
		return -1;
	*result = geometry;
	return 0;
}

#endif
