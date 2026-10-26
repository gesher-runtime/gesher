// Boundary oracle from pinned ICU4X reference ci 40374405950; see probes/native3/unicode.
struct UnicodeSample { uint32_t codepoint; uint32_t values[14]; };
static constexpr UnicodeSample kUnicodeSamples[] = {
    {0x0000000bu, {0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 11u, 11u}},
    {0x0000001cu, {0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 28u, 28u}},
    {0x00000345u, {1u, 1u, 0u, 0u, 0u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 837u, 921u}},
    {0x00002160u, {1u, 1u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 8560u, 8544u}},
    {0x00002018u, {0u, 0u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 0u, 8216u, 8216u}},
    {0x00001c89u, {1u, 1u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 7306u, 7305u}},
    {0x00010d50u, {1u, 1u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 68976u, 68944u}},
    {0x00010400u, {1u, 1u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 66600u, 66560u}},
    {0x0001e900u, {1u, 1u, 0u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 125218u, 125184u}},
    {0x0000d800u, {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 55296u, 55296u}},
    {0xffffffffu, {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 4294967295u, 4294967295u}},
};
