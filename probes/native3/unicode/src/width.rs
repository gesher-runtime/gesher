/*
 * Copyright (C) 2017 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// Derived from android-16.0.0_r4 libc/bionic/wcwidth.cpp, using the
// unmodified pinned ICU4X wrapper and the constants in libc/private/icu4x.h.
use crate::bionic_icu4x::*;

pub fn width(c: u32, unsigned_wchar: bool) -> i32 {
    if (0x20..0x7f).contains(&c) { return 1; }
    if c == 0 { return 0; }
    // Android x86_64 uses signed wchar32; arm64 uses unsigned wchar32.
    if c < 0x20 || (!unsigned_wchar && c >= 0x80000000) || (0x7f..=0xa0).contains(&c) { return -1; }
    match __icu4x_bionic_general_category(c) {
        15 => return -1,
        6 | 7 => return 0,
        16 => return if c == 0xad { 1 } else { 0 },
        _ => (),
    }
    match __icu4x_bionic_hangul_syllable_type(c) {
        2 | 3 => return 0,
        1 | 4 | 5 => return 2,
        _ => (),
    }
    if __icu4x_bionic_is_default_ignorable_code_point(c) { return 0; }
    if (0x3248..=0x324f).contains(&c) || (0x4dc0..=0x4dff).contains(&c) { return 2; }
    match __icu4x_bionic_east_asian_width(c) {
        0 | 1 | 2 | 4 => 1,
        3 | 5 => 2,
        _ => 0,
    }
}

pub fn print_reference(unsigned_wchar: bool) {
    let mut hash = 0xcbf29ce484222325u64;
    for c in (0..=0x10ffff).chain([0x110000, 0x7fffffff, 0x80000000, 0xffffffff]) {
        for byte in width(c, unsigned_wchar).to_le_bytes() {
            hash = (hash ^ u64::from(byte)).wrapping_mul(0x100000001b3);
        }
    }
    let prefix = if unsigned_wchar { "WIDTH_UNSIGNED" } else { "WIDTH" };
    println!("{prefix}_REFERENCE wcwidth {hash:016x}");
    for c in [0, 0x1f, 0x20, 0x7f, 0x9f, 0xa0, 0xa1, 0xad, 0x300, 0x345,
              0x34f, 0x378, 0x61c, 0x903, 0x115f, 0x1160, 0x11a8, 0x200d,
              0x3000, 0x3248, 0x4dc0, 0xac00, 0xac01, 0xd800, 0x16fe4,
              0x1d167, 0x1f600, 0x1fae9, 0x10ffff, 0x110000, 0x7fffffff,
              0x80000000, 0xffffffff] {
        println!("{prefix}_SAMPLE {c:08x} {}", width(c, unsigned_wchar));
    }
    println!("{prefix}_REFERENCE ICU4X 2.0.0-beta2: 1114112 code points and four invalid values");
}
