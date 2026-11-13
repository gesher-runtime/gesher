#define _GNU_SOURCE 1
#include "traversal_contract_test.inc"
__attribute__((visibility("default"))) int gesher_selftest(void) { return traversal_run(); }
