// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

// The unmodified pinned wrapper also exports width/Hangul properties; this
// reference exercises the wctype contract only.
#[allow(dead_code)]
mod bionic_icu4x;

const NAMES: [&str; 14] = [
    "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print",
    "punct", "space", "upper", "xdigit", "tolower", "toupper",
];

// android-16.0.0_r4 libc/bionic/wctype.cpp uses C-locale byte predicates for
// ASCII and these exact ICU4X properties/category bounds for all other values.
fn values(c: u32) -> [u32; 14] {
    use bionic_icu4x::*;
    if c < 128 {
        let b = c as u8;
        return [
            b.is_ascii_alphanumeric() as u32,
            b.is_ascii_alphabetic() as u32,
            (b == b' ' || b == b'\t') as u32,
            (b <= 31 || b == 127) as u32,
            b.is_ascii_digit() as u32,
            b.is_ascii_graphic() as u32,
            b.is_ascii_lowercase() as u32,
            (32..=126).contains(&b) as u32,
            b.is_ascii_punctuation() as u32,
            (b == b' ' || (9..=13).contains(&b)) as u32,
            b.is_ascii_uppercase() as u32,
            b.is_ascii_hexdigit() as u32,
            b.to_ascii_lowercase() as u32,
            b.to_ascii_uppercase() as u32,
        ];
    }
    let category = __icu4x_bionic_general_category(c);
    [
        __icu4x_bionic_is_alnum(c) as u32,
        __icu4x_bionic_is_alphabetic(c) as u32,
        __icu4x_bionic_is_blank(c) as u32,
        (category == 15) as u32,
        (category == 9) as u32,
        __icu4x_bionic_is_graph(c) as u32,
        __icu4x_bionic_is_lowercase(c) as u32,
        __icu4x_bionic_is_print(c) as u32,
        (19..=23).contains(&category) as u32,
        __icu4x_bionic_is_white_space(c) as u32,
        __icu4x_bionic_is_uppercase(c) as u32,
        __icu4x_bionic_is_xdigit(c) as u32,
        __icu4x_bionic_to_lower(c),
        __icu4x_bionic_to_upper(c),
    ]
}

fn main() {
    let extra = [0x110000, 0x7fffffff, 0x80000000, 0xffffffff];
    let mut hashes = [0xcbf29ce484222325u64; 14];
    for cp in (0..=0x10ffff).chain(extra) {
        for (i, value) in values(cp).iter().enumerate() {
            for byte in value.to_le_bytes() {
                hashes[i] = (hashes[i] ^ u64::from(byte)).wrapping_mul(0x100000001b3);
            }
        }
    }
    for (name, hash) in NAMES.iter().zip(hashes) {
        println!("REFERENCE {name} {hash:016x}");
    }
    for cp in [0x0b, 0x1c, 0x345, 0x2160, 0x2018, 0x1c89, 0x10d50,
               0x10400, 0x1e900, 0xd800, 0xffffffff] {
        println!("SAMPLE {cp:08x} {:?}", values(cp));
    }
    println!("REFERENCE ICU4X 2.0.0-beta2: 1114112 code points and four invalid values, 14 outputs");
}
