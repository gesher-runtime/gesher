// gesher: C interface used only by the independent ARM32 ABI oracle.
#include <gesher/arm32_abi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int gesher_qa_pack(const int* kinds, const uint64_t* bits, size_t count,
                             uint32_t* core, uint32_t* stack, size_t capacity,
                             size_t* stack_words) {
  std::vector<gesher::arm32::Value> arguments;
  for (size_t i = 0; i < count; ++i)
    arguments.push_back({static_cast<gesher::arm32::ValueKind>(kinds[i]), bits[i]});
  gesher::arm32::CallFrame frame;
  if (!gesher::arm32::PackArguments(arguments, &frame) || frame.stack.size() > capacity)
    return 0;
  std::copy(frame.core.begin(), frame.core.end(), core);
  std::copy(frame.stack.begin(), frame.stack.end(), stack);
  *stack_words = frame.stack.size();
  return 1;
}

extern "C" int gesher_qa_decode(int kind, uint32_t low, uint32_t high, uint64_t* bits) {
  gesher::arm32::Value value;
  if (!gesher::arm32::DecodeReturn(static_cast<gesher::arm32::ValueKind>(kind),
                                  {low, high}, &value)) return 0;
  *bits = value.bits;
  return 1;
}
