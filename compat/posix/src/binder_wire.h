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
//   fd_description_count, owned data/offset bytes, buffers_size extra bytes
//   and the FileDescription side table follow. PTR buffer
//   addresses are relative offsets in extra bytes. A failed user copy sends
//   no body and is reported through the ordinary BR_FAILED_REPLY path.
//   BC_FREE_BUFFER                cmd, the broker's 64-bit buffer id
// and the return stream likewise:
//   BR_TRANSACTION, BR_REPLY      cmd, binder_transaction_data (pointers 0),
//                                 the 64-bit buffer id, extra_size (u64),
//                                 fd_description_count (u32), then data,
//                                 offsets, owned extra bytes and FD table;
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
constexpr uint32_t kVersion = 4;

// One entry for every FD object, in object-offset order. The file HANDLE still
// occupies pad_binder on the wire; this side table carries the exact shared
// open description without reusing Android's cookie or pad_flags fields.
// A successful transaction body begins with count (u32); these entries follow
// data, offsets and extra bytes. A delivered reply uses the same representation.
struct FileDescription {
  uint64_t object_offset;
  uint64_t shared_handle;
  uint32_t kind;
  uint32_t reserved;
};
// Socket handles carry their existing ADR 0088 metadata in the FD object,
// not an OFD mapping. Keep this distinct from the unshareable-resource kind.
constexpr uint32_t kSocketFileDescription = 4;
// One transaction cannot install more than the receiver's UCRT descriptor
// ceiling. Bound metadata and native pin allocations before capturing handles.
constexpr uint32_t kMaxFileDescriptions = 8192;

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
  // -> {buffer id}; <- {status}. Validate pending delivery before local
  // installation. FD staging handles remain broker-owned even if this ACK
  // is lost; the receiver adopts separate DuplicateHandle copies only.
  kClaimBuffer = 5,
  // -> {buffer id, accepted (u32)}; <- {status}. A failed allocation/user
  // copy abandons the delivery, unwinds its transaction and releases it.
  kFinishBuffer = 6,

  // The descriptor broker (ADR 0088 section 3): SCM_RIGHTS on Gesher's AF_UNIX
  // socketpairs. A process opens one connection for it; the broker knows the
  // caller by the pipe's client process id, not by anything it claims.
  // -> {magic, kRightsVersion}; <- {status}
  kHelloRights = 7,
  // Hands descriptors to the broker, which duplicates their handles into itself.
  // -> {queue (u64): 0, or a stream connection's key; position (u64): with a
  //     queue, where the bytes sent with the group begin in that direction of
  //     the connection (kStreamPositionsPrefix); count (u32);
  //     count x {handle (u64), metadata (u64), shared_handle (u64), kind (u32)}}
  // <- {status, token (2 x u64)}. With a queue, the group also waits in that
  // connection's queue (kRightsTake), in order.
  kRightsDeposit = 8,
  // Moves a deposited group into the caller (closing the broker's handles).
  // -> {token}; <- {status, count, count x {handle, metadata, shared_handle, kind}}
  kRightsClaim = 9,
  // Moves the groups waiting in a stream connection's queue whose position is
  // below `limit` (what the receiver has read) into the caller, in order; the
  // others keep waiting.
  // -> {queue, limit (u64)}; <- {status, count, count x {handle, metadata, shared_handle, kind}}
  kRightsTake = 10,
  // Takes a group back out of its queue (its sendmsg failed) and closes it.
  // -> {token}; <- {status}: -ENOENT if a receiver already took it.
  kRightsCancel = 11,
};

// The descriptor broker's protocol version (kHelloRights), apart from kVersion:
// binderd and the processes that send or receive descriptors must agree on it;
// other binder clients need not.
constexpr uint32_t kRightsVersion = 5;

// The broker's doorbell for stream queues: a shared section named
// kRightsDoorbellPrefix + device, of kRightsDoorbellSlots uint64 values, one
// per receiving loopback port (a queue's key >> 16): 0 when no group waits for
// that port, else 1 + the lowest position of the groups that wait. Only the
// broker writes it; a read asks the broker only once it has read past such a
// position.
constexpr const wchar_t kRightsDoorbellPrefix[] = L"Local\\gesher-unix-rights-";
constexpr uint32_t kRightsDoorbellSlots = 65536;
// Stream positions: a shared section named kStreamPositionsPrefix + device, of
// kRightsDoorbellSlots {sent, received} uint64 pairs, one per stream pair end,
// by its local loopback port: the bytes written into the connection from that
// end, and the bytes read from it there. Every process that holds the end
// keeps them, so they survive the end's moves between processes (Binder,
// SCM_RIGHTS, exec); socketpair starts both ends at 0, and the broker keeps the
// section while it runs. A stream group's position is its sender's `sent`
// before the bytes sent with it, and the read whose `received` passes that
// position takes it: Linux delivers descriptors with the first byte sent with
// them, never before.
constexpr const wchar_t kStreamPositionsPrefix[] = L"Local\\gesher-unix-streams-";
struct StreamPositions {
  uint64_t sent;
  uint64_t received;
};
// Linux's SCM_MAX_FD, and the broker's limit of descriptors in flight.
constexpr uint32_t kRightsMaxPerMessage = 253;
constexpr uint32_t kRightsMaxInFlight = 65536;

// The pipe name of a device: \\.\pipe\gesher-binder-<device>.
constexpr const wchar_t kPipePrefix[] = L"\\\\.\\pipe\\gesher-binder-";

}  // namespace binder_wire
}  // namespace gesher
