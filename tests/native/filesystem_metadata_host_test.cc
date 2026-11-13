#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filesystem_metadata_contract_test.inc"

int main() { return filesystem_metadata_contracts() ? 1 : 0; }
