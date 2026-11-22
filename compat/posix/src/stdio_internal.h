// Shared by path-based stream creation and the host/guest stdio entry points.
#pragma once
#include <gesher/stdio.h>

namespace gesher::stdio {
// The single Android mode parser is implemented beside fopen/freopen.
bool ParseMode(const char* mode, int* flags, char crt_mode[4]);
// Allocate bookkeeping before transferring the descriptor to a FILE token.
FILE* OpenDescriptor(int fd, const char* crt_mode, int flags);
// The caller holds StreamGuard for lifecycle operations.
int Register(FILE* stream, int access_mode);
int FlushOutput(FILE* stream);
void Forget(FILE* stream);
int PrepareReopenedStream(FILE* stream, int flags);
}
