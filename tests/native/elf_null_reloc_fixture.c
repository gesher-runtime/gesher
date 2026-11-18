/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * An ordinary global import with no DT_NEEDED provider: the already loaded
 * versioned provider's default IFUNC definition legitimately resolves to zero. */
extern int getpid(void);
void* gv_null_import(void) { return (void*)&getpid; }
