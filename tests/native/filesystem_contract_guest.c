#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filesystem_metadata_contract_test.inc"

/* Loaded by the existing x86_64/ARM64 ELF test executables. */
int gesher_selftest(void) { return filesystem_metadata_contracts(); }

#ifdef GESHER_FILESYSTEM_REFERENCE_MAIN
int main(void) { return gesher_selftest() ? 1 : 0; }
#endif
