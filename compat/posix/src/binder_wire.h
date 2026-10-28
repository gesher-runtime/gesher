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
//   every transaction header is followed by copy_error (s32); on success,
//   owned data/offset bytes and buffers_size extra bytes follow. PTR buffer
//   addresses are relative offsets in extra bytes. A failed user copy sends
//   no body and is reported through the ordinary BR_FAILED_REPLY path.
//   BC_FREE_BUFFER                cmd, the broker's 64-bit buffer id
// and the return stream likewise:
//   BR_TRANSACTION, BR_REPLY      cmd, binder_transaction_data (pointers 0),
//                                 the 64-bit buffer id, extra_size (u64),
//                                 then data, offsets and owned extra bytes;
//                                 binder.cc copies them
//                                 into one transaction allocation and rebases
//                                 PTR objects and their parent pointer slots
// Everything else is the command and its _IOC_SIZE argument bytes.
#pragma once

#include <stdint.h>

namespace gesher {
namespace binder_wire {

constexpr uint32_t kMagic = 0x47424452;  // "GBDR"
// Both endpoints must change together; an old endpoint rejects the hello.
constexpr uint32_t kVersion = 3;
constexpr uint32_t kPeerChallenge = 0x47504348;
constexpr uint32_t kPeerAck = 0x47504143;

enum Op : uint32_t {
  // -> {magic, version, pid, uid, birth (u64)}; <- challenge then {status, proc id, key (u64)}
  kHelloProc = 1,
  // -> {magic, version, proc id, key (u64), tid}; <- challenge then {status}
  kHelloThread = 2,
  // -> {read_size (u64), write stream}; <- {status (-errno or 0),
  //    commands consumed (u32), read stream}
  kWriteRead = 3,
  // -> {request, argument bytes}; <- {status (-errno or 0), argument bytes}
  kIoctl = 4,
  // -> {buffer id}; <- {status}. Validate pending delivery before local
  // installation. FD staging handles remain broker-owned even if this ACK
  // is lost; the receiver adopts separate DuplicateHandle copies only.
  kClaimBuffer = 5,
  // -> {buffer id, accepted (u32)}; <- {status}. A failed allocation/user
  // copy abandons the delivery, unwinds its transaction and releases it.
  kFinishBuffer = 6,
};

// The pipe name of a device: \\.\pipe\gesher-binder-<device>.
constexpr const wchar_t kPipePrefix[] = L"\\\\.\\pipe\\gesher-binder-";

}  // namespace binder_wire
}  // namespace gesher
