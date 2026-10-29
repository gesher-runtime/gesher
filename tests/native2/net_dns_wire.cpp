#include <arpa/nameser.h>
#include <resolv.h>
#include <cerrno>
#include <initializer_list>
#include <cstdio>
#include <cstring>

#ifdef __ANDROID__
// Bionic resolver private ABI; not declared by the public NDK headers.
extern "C" int __dn_skipname(const unsigned char*, const unsigned char*);
#define dn_skipname __dn_skipname
#endif

#if defined(__ANDROID__) && defined(__aarch64__)
static_assert(static_cast<char>(-1) > 0, "Android ARM64 default char model");
#elif defined(__ANDROID__) && defined(__x86_64__)
static_assert(static_cast<char>(-1) < 0, "Android x86-64 default char model");
#endif
// Exact pinned decode_bitstring uses plain char for the final partial byte.
static constexpr const char* bit_text = static_cast<char>(-1) < 0 ? "\\[x7a8/5]" : "\\[xa8/5]";

static int failures;
static int rows;
static void bitdiag(const char* label, int api, size_t capacity, int rc, int error, const unsigned char* p) {
  std::fprintf(stderr, "DNS_BIT_DIAG label=%s api=%d capacity=%zu rc=%d errno=%d bytes=", label, api, capacity, rc, error);
  size_t limit = capacity < 16 ? capacity : 16;
  for (size_t i = 0; i < limit; ++i) { std::fprintf(stderr, "%02x", p[i]); if (p[i] == 0) break; }
  std::fputc('\n', stderr);
}
static void check(bool ok, const char* label) {
  if (!ok) { std::printf("FAIL DNS: %s\n", label); ++failures; }
}
static void decode(const char* id, const unsigned char* packet, size_t length,
                   size_t offset, const char* text, int consumed, size_t capacity = 1024) {
  struct { unsigned char pre[8]; unsigned char data[1024]; unsigned char post[8]; } out;
  for (int api = 0; api < 3; ++api) {
    std::memset(&out, 0xa5, sizeof(out));
    errno = 0;
    int rc = api == 0 ? ns_name_uncompress(packet, packet + length, packet + offset,
                                            reinterpret_cast<char*>(out.data), capacity) :
             api == 1 ? dn_expand(packet, packet + length, packet + offset,
                                   reinterpret_cast<char*>(out.data), static_cast<int>(capacity)) :
                        ns_name_unpack(packet, packet + length, packet + offset, out.data, capacity);
    int error = errno;
    if (std::strcmp(id, "bitstring-5") == 0 && api < 2) bitdiag(id, api, capacity, rc, error, out.data);
    bool guard = true;
    for (unsigned char b : out.pre) guard &= b == 0xa5;
    for (unsigned char b : out.post) guard &= b == 0xa5;
    for (size_t i = capacity; i < sizeof(out.data); ++i) guard &= out.data[i] == 0xa5;
    check(guard, "output canary");
    if (consumed < 0) check(rc == -1 && error == EMSGSIZE, id);
    else {
      check(rc == consumed && error == 0, id);
      if (api < 2) check(std::strcmp(reinterpret_cast<char*>(out.data),
                                    api == 1 && std::strcmp(text,".") == 0 ? "" : text) == 0, id);
      else {
        char rendered[1024] = {};
        int n = ns_name_ntop(out.data, rendered, sizeof(rendered));
        if (std::strcmp(id, "bitstring-5") == 0) bitdiag("bitstring-ntop", api, sizeof(rendered), n, errno, reinterpret_cast<unsigned char*>(rendered));
        check(n == static_cast<int>(std::strlen(text) + 1) && std::strcmp(rendered,text) == 0, id);
      }
    }
    std::printf("DNS %s api=%d rc=%d errno=%d guard=%d\n",id,api,rc,error,guard);
    ++rows;
  }
}
int main(int argc, char** argv) {
  const bool negative = argc > 1 && std::strcmp(argv[1],"--negative") == 0;
  const bool extended = argc > 1 && std::strcmp(argv[1],"--extended") == 0;
  const bool safety = argc > 1 && std::strcmp(argv[1],"--safety") == 0;
  const unsigned char root[] = {0};
  const unsigned char ordinary[] = {3,'w','w','w',7,'e','x','a','m','p','l','e',3,'c','o','m',0};
  const unsigned char compressed[] = {7,'e','x','a','m','p','l','e',0,3,'w','w','w',0xc0,0};
  const unsigned char binary[] = {5,'.',0,'\\',255,' ',0};
  decode("root",root,sizeof(root),0,".",1);
  decode("ordinary",ordinary,sizeof(ordinary),0,"www.example.com",17);
  decode("compressed",compressed,sizeof(compressed),9,"www.example",6);
  decode("binary",binary,sizeof(binary),0,"\\.\\000\\\\\\255\\032",7);
  decode("exact",ordinary,sizeof(ordinary),0,"www.example.com",17,17);
  // A 1-byte output cannot hold either root presentation or the ordinary wire name.
  decode("tiny",ordinary,sizeof(ordinary),0,"",-1,1);
  decode("zero",ordinary,sizeof(ordinary),0,"",-1,0);
  const unsigned char bad[][6] = {{0xc0,0},{0xc0,2,0xc0,0},{0xc0,0xff},
                                 {0xc0},{0x80,0},{0x42,0},{4,'a','b',0}};
  const size_t lengths[] = {2,4,2,1,2,2,4};
  for (size_t i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
    char label[32]; std::snprintf(label,sizeof(label),"malformed-%zu",i);
    decode(label,bad[i],lengths[i],0,"",-1);
  }
  for (size_t cut=0;cut<sizeof(ordinary);++cut) {
    char label[32]; std::snprintf(label,sizeof(label),"truncated-%zu",cut);
    decode(label,ordinary,cut,0,"",-1);
  }
  for (const auto* wire : {root,ordinary,compressed + 9}) {
    const unsigned char* end = wire == root ? root+sizeof(root) :
                              wire == ordinary ? ordinary+sizeof(ordinary) : compressed+sizeof(compressed);
    const unsigned char* cursor = wire;
    errno = 0;
    int rc = ns_name_skip(&cursor,end);
    check(rc == 0 && cursor == end && errno == 0,"skip valid");
    int n = dn_skipname(wire,end);
    check(n == end-wire,"dn_skip valid");
    std::printf("DNS skip bytes=%d rc=%d errno=%d\n",n,rc,errno); ++rows;
  }
  unsigned char limit[256];
  size_t at=0;
  for (int n : {63,63,63,61}) { limit[at++]=static_cast<unsigned char>(n); std::memset(limit+at,'x',n); at+=n; }
  limit[at++]=0;
  unsigned char unpacked[255];
  check(at==255 && ns_name_unpack(limit,limit+at,limit,unpacked,sizeof(unpacked))==255,"maximum name");
  check(std::memcmp(limit,unpacked,255)==0,"maximum wire independent bytes");
  // All three APIs must reject a wire name too large for the 255-byte buffer.
  limit[at-2]='x'; limit[at-1]='x'; limit[0]=63;
  unsigned char oversized[257];
  std::memcpy(oversized,limit,255); oversized[192]=63;
  oversized[255]='x'; oversized[256]=0;
  errno=0;
  check(ns_name_unpack(oversized,oversized+sizeof(oversized),oversized,unpacked,sizeof(unpacked))==-1 && errno==EMSGSIZE,
        "wire output overflow");
  char presentation[4] = {'!','!','!','!'};
  errno=0;
  check(ns_name_ntop(root,presentation,0)==-1 && errno==EMSGSIZE && presentation[0]=='!',"ntop zero capacity");
  const unsigned char pointer[]={0xc0,0};
  errno=0;
  check(ns_name_ntop(pointer,presentation,sizeof(presentation))==-1 && errno==EMSGSIZE,"ntop compressed rejection");
  if (extended) {
    const unsigned char bits[]={0x41,5,0xaf,0};
    decode("bitstring-5",bits,sizeof(bits),0,bit_text,4);
    const unsigned char lengths[] = {1, 4, 6, 7, 8};
    const char* signed_text[] = {"\\[x8/1]", "\\[xa/4]", "\\[x3ac/6]", "\\[x1ae/7]", "\\[xaf/8]"};
    const char* unsigned_text[] = {"\\[x8/1]", "\\[xa/4]", "\\[xac/6]", "\\[xae/7]", "\\[xaf/8]"};
    for (size_t i = 0; i < sizeof(lengths); ++i) {
      const unsigned char wire[] = {0x41, lengths[i], 0xaf, 0};
      char id[32]; std::snprintf(id, sizeof(id), "bitstring-%u", lengths[i]);
      decode(id, wire, sizeof(wire), 0, static_cast<char>(-1) < 0 ? signed_text[i] : unsigned_text[i], 4);
    }
    // Discarded low bits must not influence a partial-byte rendering.
    for (unsigned length : {5u, 6u, 7u}) {
      for (unsigned byte : {0u, 1u}) {
        const unsigned char wire[] = {0x41, static_cast<unsigned char>(length),
                                      static_cast<unsigned char>(byte), 0};
        char id[40], expected[40];
        std::snprintf(id, sizeof(id), "bitstring-%u-low-%u", length, byte);
        std::snprintf(expected, sizeof(expected), "\\[x00/%u]", length);
        decode(id, wire, sizeof(wire), 0, expected, 4);
      }
    }
    unsigned char full_bits[35] = {0x41, 0};
    std::memset(full_bits + 2, 0xaf, 32);
    char full_text[80] = "\\[x";
    for (size_t i = 0; i < 32; ++i) std::strcat(full_text, "af");
    std::strcat(full_text, "/256]");
    decode("bitstring-256", full_bits, sizeof(full_bits), 0, full_text, 35);
    for (size_t capacity=0;capacity<=12;++capacity) {
      for (int api=0;api<3;++api) {
        unsigned char out[24]; std::memset(out,0xa5,sizeof(out)); errno=0;
        char* dst=reinterpret_cast<char*>(out+4);
        int rc=api==0 ? ns_name_ntop(bits,dst,capacity) :
               api==1 ? ns_name_uncompress(bits,bits+sizeof(bits),bits,dst,capacity) :
                        dn_expand(bits,bits+sizeof(bits),bits,dst,static_cast<int>(capacity));
        int error=errno;
        bitdiag("bit-cap",api,capacity,rc,error,out+4);
        bool guard=true;
        for (size_t i=0;i<4;++i)guard &= out[i]==0xa5;
        for (size_t i=4+capacity;i<sizeof(out);++i)guard &= out[i]==0xa5;
        check(guard,"bitstring capacity guard");
        check(capacity<10 ? rc==-1 && error==EMSGSIZE :
              rc==(api==0 ? static_cast<int>(std::strlen(bit_text)+1) : 4) && error==0 && std::strcmp(dst,bit_text)==0,
              "pinned bitstring conservative headroom");
        std::printf("DNS bit-cap capacity=%zu api=%d rc=%d errno=%d guard=%d\n",capacity,api,rc,error,guard);
        ++rows;
      }
    }
  }
  if (safety) {
    const unsigned char missing_header[]={0x41};
    decode("missing-bitstring-header",missing_header,sizeof(missing_header),0,"",-1);
    const unsigned char missing_root[]={1,'a'};
    for (const auto* wire : {root,missing_header,missing_root,pointer}) {
      const unsigned char* end = wire==root ? root : wire==missing_header ? wire+1 : wire==missing_root ? wire+2 : wire+1;
      const unsigned char* cursor=wire;
      errno=0;
      check(ns_name_skip(&cursor,end)==-1 && errno==EMSGSIZE && cursor==wire,"skip truncated/cursor preserved");
      errno=0;
      check(dn_skipname(wire,end)==-1 && errno==EMSGSIZE,"dn_skip truncated");
    }
    std::printf("DNS safety skip cases=4 preserved=4\n"); ++rows;
  }
  if (negative) decode("negative corruption rejected",ordinary,sizeof(ordinary),0,"www.example.com",999);
  std::printf("DNS all done rows=%d failures=%d\n",rows,failures);
  return failures ? 1 : 0;
}
