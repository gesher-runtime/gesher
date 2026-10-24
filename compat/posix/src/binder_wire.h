// gesher-base: the protocol between a process's /dev/binder (binder.cc) and
// binderd, the broker that plays the binder driver's part
// (compat/binder/binderd.cc, docs/decisions/0015-services-binder-ipc.md).
//
// Every message is a 32-bit little-endian length followed by that many bytes,
// over a byte-mode named pipe, \\.\pipe\gesher-binder-<device> (the device is
// $GESHER_BINDER). A process opens one connection when it opens /dev/binder
// (kHelloProc), which stays idle until the process closes the fd or exits:
// then the broker releases the process, as the kernel does in binder_release.
// Each thread that issues an ioctl opens a connection of its own
// (kHelloThread), on which every request gets one reply: the broker's thread
// for that connection is the kernel's context for that binder thread, and
// blocks where the kernel would.
//
// BINDER_WRITE_READ carries the command stream as libbinder wrote it, except
// that the broker cannot read the sender's memory:
//   BC_TRANSACTION, BC_REPLY      cmd, binder_transaction_data (pointers 0),
//                                 then data_size bytes and offsets_size bytes
//   BC_TRANSACTION_SG, BC_REPLY_SG  the same with binder_transaction_data_sg
//   BC_FREE_BUFFER                cmd, the broker's 64-bit buffer id
// and the return stream likewise:
//   BR_TRANSACTION, BR_REPLY      cmd, binder_transaction_data (pointers 0),
//                                 the 64-bit buffer id, then the data and
//                                 offsets, which binder.cc copies into the
//                                 process's binder mapping
// Everything else is the command and its _IOC_SIZE argument bytes.
#pragma once

#include <stdint.h>

namespace gesher {
namespace binder_wire {

constexpr uint32_t kMagic = 0x47424452;  // "GBDR"
constexpr uint32_t kVersion = 1;

enum Op : uint32_t {
  // -> {magic, version, pid, uid}; <- {status, proc id, key (u64)}
  kHelloProc = 1,
  // -> {magic, version, proc id, key (u64), tid}; <- {status}
  kHelloThread = 2,
  // -> {read_size (u64), write stream}; <- {status (-errno or 0),
  //    commands consumed (u32), read stream}
  kWriteRead = 3,
  // -> {request, argument bytes}; <- {status (-errno or 0), argument bytes}
  kIoctl = 4,
};

// The pipe name of a device: \\.\pipe\gesher-binder-<device>.
constexpr const wchar_t kPipePrefix[] = L"\\\\.\\pipe\\gesher-binder-";

}  // namespace binder_wire
}  // namespace gesher
