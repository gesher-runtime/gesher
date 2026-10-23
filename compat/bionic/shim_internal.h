// gesher bionic shim: what the shim's files share.
#pragma once

#include "abi_types.h"

#include <stdint.h>

#include <string>
#include <unordered_map>

namespace gesher::bionic {

// Symbol name -> address of the System V entry point (or data object).
using SymbolTable = std::unordered_map<std::string, void*>;

// Each part of the shim adds its symbols.
void RegisterLibc(SymbolTable& table);     // shim.cc
void RegisterStdio(SymbolTable& table);    // stdio_bionic.cc
void RegisterPthread(SymbolTable& table);  // pthread_bionic.cc

// Linux errno values the shim returns where the CRT's numbering differs
// (gesher-base's <errno.h> already uses Linux's; these are for clarity).
constexpr int kEPERM = 1;
constexpr int kESRCH = 3;
constexpr int kEAGAIN = 11;
constexpr int kENOMEM = 12;
constexpr int kEBUSY = 16;
constexpr int kEINVAL = 22;
constexpr int kEDEADLK = 35;
constexpr int kENOSYS = 38;
constexpr int kETIMEDOUT = 110;

// A Linux struct timespec as the .so lays it out (64-bit tv_nsec).
struct LinuxTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

// CLOCK_REALTIME now, as a Linux timespec.
LinuxTimespec RealtimeNow();
// Milliseconds from now until `abstime` on CLOCK_REALTIME (or the monotonic
// clock), clamped at 0; for timed waits.
uint32_t MillisUntil(const LinuxTimespec* abstime, bool monotonic);

}  // namespace gesher::bionic
