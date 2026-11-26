// gesher: execute real A32 guest instructions through the checked adapter.
#include <gesher/arm32_executor.h>
#include <gesher/arm32_elf.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace gesher::arm32;
namespace {
unsigned checks;
void Check(bool value, const char* label) {
  ++checks;
  if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
}
constexpr uint32_t kCode = 0x20000;
constexpr uint32_t kData = 0x30000;
constexpr uint32_t kStack = 0x100000;
constexpr uint32_t kStackSize = 0x10000;
constexpr uint32_t kStackTop = kStack + kStackSize;
constexpr auto kRW = MemoryPermission::kRead | MemoryPermission::kWrite;
constexpr auto kRX = MemoryPermission::kRead | MemoryPermission::kExecute;

CallFrame Arguments(std::initializer_list<Value> values) {
  CallFrame result;
  std::string error;
  Check(PackArguments(values, &result, &error), "pack real guest arguments");
  return result;
}
void Code(GuestMemory& memory, std::initializer_list<uint32_t> words) {
  Check(memory.Protect(kCode, 4096, kRW), "make code writable");
  uint32_t pc = kCode;
  for (auto word : words) { Check(memory.Write32(pc, word), "write code"); pc += 4; }
  Check(memory.Protect(kCode, 4096, kRX), "publish code RX");
}
void Returned(const ExecutionResult& result, uint32_t expected, const char* label) {
  if (!result.returned()) std::cerr << "execution: " << result.error << " PC=" << std::hex << result.pc << std::dec << '\n';
  Check(result.returned() && result.registers[0] == expected, label);
}

void Instructions() {
  GuestMemory memory;
  Check(memory.Map(kCode, 4096, kRW), "map code");
  Check(memory.Map(kData, 4096, kRW), "map data");
  Check(memory.Map(kStack, kStackSize, kRW), "map stack");
  Executor executor(memory);
  Code(memory, {0xe0800001, 0xe12fff1e});  // ADD r0,r0,r1; BX lr.
  Returned(executor.Call(kCode, Arguments({Value::Signed32(13), Value::Signed32(29)}), kStackTop),
           42, "ARM addition and LR return");
  Code(memory, {0x47701840});  // Thumb ADDS r0,r0,r1; BX lr.
  Returned(executor.Call(kCode | 1, Arguments({Value::Signed32(20), Value::Signed32(22)}), kStackTop),
           42, "Thumb addition and ARM interworking return");
  Code(memory, {0xe0900002, 0xe0a11003, 0xe12fff1e}); // ADDS/ADC 64-bit pairs.
  auto wide = executor.Call(kCode, Arguments({Value::Unsigned64(0xffffffff), Value::Unsigned64(2)}), kStackTop);
  Check(wide.returned() && wide.registers[0] == 1 && wide.registers[1] == 1, "64-bit carry result");
  Code(memory, {0xe59d0000, 0xe12fff1e}); // LDR r0,[sp]; BX lr.
  Returned(executor.Call(kCode, Arguments({Value::Signed32(1), Value::Signed32(2),
           Value::Signed32(3), Value::Signed32(4), Value::Signed32(55)}), kStackTop), 55, "stack argument");

  Code(memory, {0xe5900000, 0xe5821000, 0xe12fff1e}); // Fault before STR side effect.
  auto fault = executor.Call(kCode, Arguments({Value::GuestPointer(0x500000), Value::Unsigned32(99),
                             Value::GuestPointer(kData)}), kStackTop);
  uint32_t stored = 123;
  Check(fault.stop == ExecutionStop::MemoryFault, "unmapped data read stops guest");
  Check(memory.Read32(kData, &stored) && stored == 0, "fault stops before later side effect");
  Code(memory, {0xe5801000, 0xe12fff1e});
  Check(memory.Protect(kData, 4096, MemoryPermission::kRead), "read only data");
  Check(executor.Call(kCode, Arguments({Value::GuestPointer(kData), Value::Unsigned32(99)}), kStackTop).stop ==
        ExecutionStop::MemoryFault, "read-only data rejects guest writes");
  Check(memory.Protect(kData, 4096, kRW), "restore data");

  Code(memory, {0xe12fff10}); // BX r0, into non-executable data.
  Check(executor.Call(kCode, Arguments({Value::GuestPointer(kData)}), kStackTop).stop ==
        ExecutionStop::MemoryFault, "data is never executed as host code");
  Code(memory, {0xeafffffe}); // B .
  Check(executor.Call(kCode, {}, kStackTop, 100).stop == ExecutionStop::BudgetExceeded,
        "infinite loop has finite budget");
  Code(memory, {0xef000000, 0xe12fff1e});
  Check(executor.Call(kCode, {}, kStackTop).stop == ExecutionStop::UnsupportedSupervisorCall,
        "unimplemented syscalls never report success");
  Code(memory, {0xe7f000f0, 0xe12fff1e}); // UDF.
  Check(executor.Call(kCode, {}, kStackTop).stop == ExecutionStop::UnsupportedInstruction,
        "undefined instruction stops");
  Code(memory, {0xe3a00001, 0xe12fff1e});
  Returned(executor.Call(kCode, {}, kStackTop), 1, "recover after faults");
  Code(memory, {0xe3a00002, 0xe12fff1e});
  Returned(executor.Call(kCode, {}, kStackTop), 2, "rewritten code invalidates old translation");
  Check(executor.Call(kCode + 2, {}, kStackTop).stop == ExecutionStop::InvalidCall, "misaligned ARM PC");
  Check(executor.Call(kCode, {}, kStackTop - 1).stop == ExecutionStop::InvalidCall, "unaligned stack");
  Check(executor.Call(kCode, {}, 0x900000).stop == ExecutionStop::InvalidCall, "unmapped empty stack");
  Check(executor.Call(kCode, {}, kStackTop, 0).stop == ExecutionStop::InvalidCall, "zero budget");
  Check(executor.Call(kCode, {}, kStackTop, std::numeric_limits<uint64_t>::max()).stop ==
        ExecutionStop::InvalidCall, "out-of-range signed tick budget rejected");
  Returned(executor.Call(kCode, {}, kStackTop, std::numeric_limits<int64_t>::max()),
           2, "largest accepted instruction budget");
  Check(memory.Map(0xffff0000, 4096, kRW), "reserved marker conflict setup");
  Check(executor.Call(kCode, {}, kStackTop).stop == ExecutionStop::InvalidCall, "reserved marker conflict rejected");
  Check(memory.Unmap(0xffff0000, 4096), "reserved marker conflict removed");

  std::string error;
  Check(executor.BindHostFunction(0x70000000,
        {ValueKind::I32, ValueKind::F64, ValueKind::U64, ValueKind::I32}, ValueKind::F64,
        [](const std::vector<Value>& args, Value* result, std::string*) {
          if (args.size() != 4 || args[0].bits != 7 || args[2].bits != 0x100000002ULL || args[3].bits != 9)
            return false;
          double number;
          std::memcpy(&number, &args[1].bits, sizeof(number));
          *result = Value::Float64(number + 2.5);
          return true;
        }, &error), "bind explicit typed host function");
  auto host = executor.Call(0x70000000, Arguments({Value::Signed32(7), Value::Float64(1.25),
                           Value::Unsigned64(0x100000002ULL), Value::Signed32(9)}), kStackTop);
  Value number;
  Check(host.returned() && DecodeReturn(ValueKind::F64, host.registers, &number) &&
        number.bits == Value::Float64(3.75).bits, "typed host call preserves softfp and 64-bit spill");
  Check(!executor.BindHostFunction(0x70000000, {}, ValueKind::Void,
        [](const auto&, Value*, std::string*) { return true; }, &error), "binding never replaces existing guest memory");
  Check(executor.BindHostFunction(0x70001000, {}, ValueKind::I32,
        [](const auto&, Value*, std::string*) -> bool { throw std::runtime_error("test host failure"); }, &error),
        "bind throwing adapter");
  Check(executor.Call(0x70001000, {}, kStackTop).stop == ExecutionStop::HostCallFailed,
        "host exception becomes explicit failure");
  Returned(executor.Call(kCode, {}, kStackTop), 2, "executor reusable after host failure");
  Check(executor.BindHostFunction(0x70002000, {}, ValueKind::I32,
        [&](const auto&, Value* result, std::string*) {
          auto nested = executor.Call(kCode, {}, kStackTop);
          *result = Value::Signed32(nested.stop == ExecutionStop::InvalidCall ? 1 : 0);
          return true;
        }, &error), "bind reentrant adapter");
  Returned(executor.Call(0x70002000, {}, kStackTop), 1, "same executor reentry rejected without deadlock");

  // An exclusive load must fault on misalignment even though an ordinary ARM
  // load from the same address is valid. Stores also need a live reservation.
  Check(memory.Write32(kData, 7), "initialize exclusive value");
  Code(memory, {0xe1901f9f, 0xe2811001, 0xe1802f91, 0xe1a00002, 0xe12fff1e});
  Returned(executor.Call(kCode, Arguments({Value::GuestPointer(kData)}), kStackTop),
           0, "LDREX and STREX succeed with reservation");
  Check(memory.Read32(kData, &stored) && stored == 8, "exclusive store updated memory");
  Code(memory, {0xe1901f9f, 0xf57ff01f, 0xe1802f91, 0xe1a00002, 0xe12fff1e});
  Returned(executor.Call(kCode, Arguments({Value::GuestPointer(kData)}), kStackTop),
           1, "CLREX invalidates exclusive reservation");
  Code(memory, {0xe1901f9f, 0xe2812001, 0xe5802000, 0xe5801000,
                0xe1802f91, 0xe1a00002, 0xe12fff1e});
  Returned(executor.Call(kCode, Arguments({Value::GuestPointer(kData)}), kStackTop),
           1, "intervening ordinary ABA stores invalidate reservation");
  Code(memory, {0xe1901f9f, 0xe1a00001, 0xe12fff1e});
  for (uint32_t offset = 1; offset < 4; ++offset)
    Check(executor.Call(kCode, Arguments({Value::GuestPointer(kData + offset)}), kStackTop).stop ==
          ExecutionStop::MemoryFault, "misaligned exclusive read faults");
  Check(memory.Write32(kData, 0xaabbccdd) && memory.Write32(kData + 4, 0x11223344),
        "ordinary unaligned load input");
  Code(memory, {0xe5900000, 0xe12fff1e});
  Returned(executor.Call(kCode, Arguments({Value::GuestPointer(kData + 1)}), kStackTop),
           0x44aabbcc, "ordinary unaligned load remains valid");

  constexpr uint32_t leaf = 0x40000;
  Check(memory.Map(leaf, 4096, kRW), "map separate cache test leaf");
  Check(memory.Write32(leaf, 0xe3a00001) && memory.Write32(leaf + 4, 0xe12fff1e) &&
        memory.Protect(leaf, 4096, kRX), "publish original leaf");
  bool revoke = false;
  Check(executor.BindHostFunction(0x70003000, {}, ValueKind::Void,
        [&](const auto&, Value* result, std::string*) {
          if (!memory.Protect(leaf, 4096, kRW)) return false;
          if (!revoke && (!memory.Write32(leaf, 0xe3a00002) || !memory.Protect(leaf, 4096, kRX)))
            return false;
          *result = {ValueKind::Void, 0};
          return true;
        }, &error), "bind code mutation adapter");
  // Preserve host/leaf in callee-saved registers; call leaf, host, then leaf.
  Code(memory, {0xe92d4070, 0xe1a04000, 0xe1a05001, 0xe12fff35,
                0xe12fff34, 0xe12fff35, 0xe8bd8070});
  auto cached_args = Arguments({Value::GuestPointer(0x70003000), Value::GuestPointer(leaf)});
  Returned(executor.Call(kCode, cached_args, kStackTop), 2, "host code rewrite invalidates cached leaf");
  revoke = true;
  Check(executor.Call(kCode, cached_args, kStackTop).stop == ExecutionStop::MemoryFault,
        "host execute-permission revocation invalidates cached leaf");

  Check(memory.Protect(leaf, 4096, kRW | MemoryPermission::kExecute) &&
        memory.Write32(leaf, 0xe3a00001), "prepare guest self-modified leaf");
  Code(memory, {0xe92d4010, 0xe1a04000, 0xe12fff34, 0xe59f100c,
                0xe5841000, 0xf57ff06f, 0xe12fff34, 0xe8bd8010, 0xe3a00002});
  Returned(executor.Call(kCode, Arguments({Value::GuestPointer(leaf)}), kStackTop),
           2, "guest ISB invalidates self-modified code");
}

void CompiledElf(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  Check(bool(stream), "open compiled ELF32 fixture");
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
  GuestMemory memory;
  Check(memory.Map(kStack, kStackSize, kRW), "compiled fixture stack");
  Check(memory.Map(kData, 4096, kRW), "compiled fixture data");
  Executor executor(memory);
  std::string error;
  Check(executor.BindHostFunction(0x70000000, {ValueKind::I32}, ValueKind::I32,
        [](const auto& values, Value* result, std::string*) {
          *result = Value::Signed32(static_cast<int32_t>(values[0].bits) * 3);
          return true;
        }, &error), "compiled fixture import binding");
  ElfImage image;
  ElfLoadOptions options;
  options.load_bias = 0x400000;
  options.resolve_import = [](std::string_view name, uint32_t* address) {
    if (name != "host_adjust") return false;
    *address = 0x70000000;
    return true;
  };
  bool loaded = LoadElf32(bytes.data(), bytes.size(), options, &image, &error);
  if (!loaded) std::cerr << "ELF: " << error << '\n';
  Check(loaded, "load real compiler ELF32");
  bool mapped = MapElfImage(image, &memory, &error);
  if (!mapped) std::cerr << "mapping: " << error << '\n';
  Check(mapped, "map real compiler ELF32");
  Check(!image.relro.empty(), "compiler fixture contains GNU RELRO");
  for (const auto& range : image.relro) {
    Check(!memory.Write32(range.address, 0), "relocated RELRO data is read only");
  }
  Check(!image.constructors.empty(), "real dynamic constructor found");
  for (auto constructor : image.constructors)
    Check(executor.Call(constructor, {}, kStackTop).returned(), "run compiled constructor");
  auto call = [&](const char* name, const CallFrame& frame) {
    uint32_t address;
    Check(image.Lookup(name, &address), name);
    auto result = executor.Call(address, frame, kStackTop);
    if (!result.returned()) std::cerr << name << ": " << result.error << '\n';
    Check(result.returned(), "compiled function returns");
    return result;
  };
  Returned(call("fixture_add", Arguments({Value::Signed32(11), Value::Signed32(13)})), 66,
           "constructor and relocated data pointer affect real code");
  auto wide = call("fixture_wide", Arguments({Value::Signed32(5), Value::Signed64(0x100000002LL),
                   Value::Signed64(0x200000003LL), Value::Signed32(7)}));
  Check(wide.registers[0] == 17 && wide.registers[1] == 3, "compiled 64-bit alignment and result");
  auto floating = call("fixture_double", Arguments({Value::Signed32(2), Value::Float64(1.25),
                       Value::Signed32(3), Value::Float64(2.5)}));
  Value double_result;
  Check(DecodeReturn(ValueKind::F64, floating.registers, &double_result) &&
        double_result.bits == Value::Float64(8.75).bits, "compiled softfp double register and stack arguments");
  auto single = call("fixture_float", Arguments({Value::Float32(1.25f), Value::Float32(2.5f)}));
  Check(single.registers[0] == Value::Float32(3.75f).bits, "compiled softfp float return");
  Check(memory.Write32(kData, 4) && memory.Write32(kData + 4, 8) && memory.Write32(kData + 8, 15),
        "compiled pointer inputs");
  Returned(call("fixture_memory", Arguments({Value::GuestPointer(kData), Value::Unsigned32(3)})), 27,
           "compiled guest pointer reads and writes");
  uint32_t sum;
  Check(memory.Read32(kData, &sum) && sum == 27, "compiled write visible in guest memory");
  Returned(call("fixture_import", Arguments({Value::Signed32(9)})), 32,
           "ELF PLT import reaches explicit host adapter");
}
}  // namespace

int main(int argc, char** argv) {
  Instructions();
  if (argc == 2) CompiledElf(argv[1]);
  std::cout << "ARM32 executor: " << checks << " checks passed\n";
  return 0;
}
