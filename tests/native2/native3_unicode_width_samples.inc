// Pinned signed/unsigned bionic width references: ci 40403945297 / 40423333599.
struct UnicodeWidthSample { uint32_t codepoint; int signed_width; int unsigned_width; };
constexpr UnicodeWidthSample kUnicodeWidthSamples[] = {
    {UINT32_C(0x00000000), 0, 0},
    {UINT32_C(0x0000001f), -1, -1},
    {UINT32_C(0x00000020), 1, 1},
    {UINT32_C(0x0000007f), -1, -1},
    {UINT32_C(0x0000009f), -1, -1},
    {UINT32_C(0x000000a0), -1, -1},
    {UINT32_C(0x000000a1), 1, 1},
    {UINT32_C(0x000000ad), 1, 1},
    {UINT32_C(0x00000300), 0, 0},
    {UINT32_C(0x00000345), 0, 0},
    {UINT32_C(0x0000034f), 0, 0},
    {UINT32_C(0x00000378), 1, 1},
    {UINT32_C(0x0000061c), 0, 0},
    {UINT32_C(0x00000903), 1, 1},
    {UINT32_C(0x0000115f), 2, 2},
    {UINT32_C(0x00001160), 0, 0},
    {UINT32_C(0x000011a8), 0, 0},
    {UINT32_C(0x0000200d), 0, 0},
    {UINT32_C(0x00003000), 2, 2},
    {UINT32_C(0x00003248), 2, 2},
    {UINT32_C(0x00004dc0), 2, 2},
    {UINT32_C(0x0000ac00), 2, 2},
    {UINT32_C(0x0000ac01), 2, 2},
    {UINT32_C(0x0000d800), 1, 1},
    {UINT32_C(0x00016fe4), 0, 0},
    {UINT32_C(0x0001d167), 0, 0},
    {UINT32_C(0x0001f600), 2, 2},
    {UINT32_C(0x0001fae9), 2, 2},
    {UINT32_C(0x0010ffff), 1, 1},
    {UINT32_C(0x00110000), 1, 1},
    {UINT32_C(0x7fffffff), 1, 1},
    {UINT32_C(0x80000000), -1, 1},
    {UINT32_C(0xffffffff), -1, 1},
};
static_assert(sizeof(wchar_t) == 4, "Android wchar32 ABI");
#if defined(__WCHAR_UNSIGNED__)
constexpr uint64_t kUnicodeWidthDigest = UINT64_C(0x3650764a32c5bf03);
constexpr int WidthExpected(const UnicodeWidthSample& sample) { return sample.unsigned_width; }
constexpr const char* kUnicodeWidthAbi = "unsigned wchar32";
#else
constexpr uint64_t kUnicodeWidthDigest = UINT64_C(0x566ac5b76f65a70b);
constexpr int WidthExpected(const UnicodeWidthSample& sample) { return sample.signed_width; }
constexpr const char* kUnicodeWidthAbi = "signed wchar32";
#endif
