// A revocable file capability shared by ordinary descriptor aliases.
#pragma once
#include <cstdint>

namespace gesher {
// Opaque process-local controller IDs; zero is never a valid controller.
// Descriptors retain their shared capability independently of the controller.
uint64_t CreateRevocableFileGroup();
int AttachRevocableFileGroup(uint64_t controller, int fd);
// Waits for already admitted I/O, then rejects all subsequent data access.
int RevokeFileGroup(uint64_t controller);
// Idempotent cleanup; callers revoke before releasing a live session.
void ReleaseRevocableFileGroup(uint64_t controller);
}  // namespace gesher
