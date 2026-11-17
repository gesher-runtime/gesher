#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "xattr_fd_contract_test.inc"
int main(void) { return xattr_fd_contracts() ? 1 : 0; }
