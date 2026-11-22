// Kernel pipe operations; no process-local peer counts or capacity estimates.
#pragma once
#include <windows.h>
#include <gesher/file_description.h>
namespace gesher {
int CreatePipeDescriptors(int descriptors[2], int flags);
int SetPipeNonBlocking(HANDLE file, bool nonblocking);
int TransferPipe(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count, bool writing);
int PollPipeHandle(const DescriptorSnapshotPtr& snapshot, short events, short* ready);
int PipeBytesAvailable(const DescriptorSnapshotPtr& snapshot, int* bytes);
}
