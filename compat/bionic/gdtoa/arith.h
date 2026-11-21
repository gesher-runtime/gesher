// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Little-endian IEEE binary64, 32-bit int; output is Android binary128 even
// on Windows LLP64. ParseBinary128 serializes all upstream shared caches.
#define IEEE_8087
#define Long int
#define INFNAN_CHECK
#define PROTO_NORMAL(symbol)
#define __BEGIN_HIDDEN_DECLS
#define __END_HIDDEN_DECLS

// Prefix the compiled upstream symbols, without interposing CRT functions.
#define __strtorQ gesher_gdtoa_strtorQ
#define __strtodg gesher_gdtoa_strtodg
#define __Balloc_D2A gesher_gdtoa_Balloc
#define __Bfree_D2A gesher_gdtoa_Bfree
#define __ULtoQ_D2A gesher_gdtoa_ULtoQ
#define __any_on_D2A gesher_gdtoa_any_on
#define __b2d_D2A gesher_gdtoa_b2d
#define __bigtens_D2A gesher_gdtoa_bigtens
#define __cmp_D2A gesher_gdtoa_cmp
#define __copybits_D2A gesher_gdtoa_copybits
#define __d2b_D2A gesher_gdtoa_d2b
#define __decrement_D2A gesher_gdtoa_decrement
#define __diff_D2A gesher_gdtoa_diff
#define __gethex_D2A gesher_gdtoa_gethex
#define __hexdig_D2A gesher_gdtoa_hexdig
#define __hexdig_init_D2A gesher_gdtoa_hexdig_init
#define __hexnan_D2A gesher_gdtoa_hexnan
#define __hi0bits_D2A gesher_gdtoa_hi0bits
#define __i2b_D2A gesher_gdtoa_i2b
#define __increment_D2A gesher_gdtoa_increment
#define __lo0bits_D2A gesher_gdtoa_lo0bits
#define __lshift_D2A gesher_gdtoa_lshift
#define __match_D2A gesher_gdtoa_match
#define __mult_D2A gesher_gdtoa_mult
#define __multadd_D2A gesher_gdtoa_multadd
#define __pow5mult_D2A gesher_gdtoa_pow5mult
#define __quorem_D2A gesher_gdtoa_quorem
#define __ratio_D2A gesher_gdtoa_ratio
#define __rshift_D2A gesher_gdtoa_rshift
#define __s2b_D2A gesher_gdtoa_s2b
#define __set_ones_D2A gesher_gdtoa_set_ones
#define __sum_D2A gesher_gdtoa_sum
#define __tens_D2A gesher_gdtoa_tens
#define __tinytens_D2A gesher_gdtoa_tinytens
#define __trailz_D2A gesher_gdtoa_trailz
#define __ulp_D2A gesher_gdtoa_ulp
