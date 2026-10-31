// Dedicated CI object: compile with the actual mman -fno-exceptions mode.
#include "../../compat/posix/src/mman_registry_internal.h"
#if defined(__cpp_exceptions)
#error dedicated caller must retain -fno-exceptions
#endif
namespace gesher_mman_internal {
PrepareStatus PrepareAliasFromNoExceptions(const AliasInput& in, PreparedAlias& out) noexcept {
  return PrepareAlias(in,out); // no allocation or exception handling in this frame
}
PrepareStatus PrepareSurgeryFromNoExceptions(const Region& source, uintptr_t start,
    uintptr_t x, uintptr_t y, size_t page, PreparedSurgery& out) noexcept {
  return PrepareSurgery(source,start,x,y,page,out);
}
PrepareStatus PreparePunchLedgerFromNoExceptions(size_t length,size_t page,uintptr_t group,
    PreparedPunchLedger& out) noexcept {
  return PreparePunchLedger(length,page,group,out);
}

}
