/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */
static int constructor_calls;
__attribute__((constructor)) static void loaded(void) { ++constructor_calls; }
int n03_constructor_calls(void) { return constructor_calls; }
