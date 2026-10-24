// Gesher: dex2oat links no libart-disassembler (art/disassembler), which the
// optimizing compiler's graph visualizer calls for --dump-cfg's disassembly;
// without it the CFG dump has no disassembly.

#include "disassembler.h"

namespace art {

extern "C" Disassembler* create_disassembler([[maybe_unused]] InstructionSet instruction_set,
                                              DisassemblerOptions* options) {
  delete options;
  return nullptr;
}

}  // namespace art
