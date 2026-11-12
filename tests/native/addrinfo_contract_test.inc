// Public numeric-only DNS adapter contracts shared with native ELF fixtures.
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

struct addrinfo_contract_row {
  int32_t flags, family, socktype, protocol;
  uint32_t address_length;
  char* canonical_name;
  struct sockaddr* address;
  struct addrinfo_contract_row* next;
};
typedef int (*addrinfo_contract_get_fn)(const char*, const char*, const struct addrinfo_contract_row*, struct addrinfo_contract_row**);
typedef int (*addrinfo_contract_network_fn)(uint64_t, const char*, const char*, const struct addrinfo_contract_row*, struct addrinfo_contract_row**);
typedef void (*addrinfo_contract_free_fn)(struct addrinfo_contract_row*);
static int addrinfo_contract_numeric_list(const struct addrinfo_contract_row* row) {
  int count = 0;
  for (; row != NULL; row = row->next) {
    if (++count > 8 || row->family != AF_INET || row->socktype != SOCK_STREAM ||
        row->protocol != IPPROTO_TCP || row->address == NULL || row->address_length != sizeof(struct sockaddr_in)) return 0;
    const struct sockaddr_in* address = (const struct sockaddr_in*)row->address;
    if (address->sin_family != AF_INET || ntohs(address->sin_port) != 80 ||
        ntohl(address->sin_addr.s_addr) != UINT32_C(0x7f000001)) return 0;
  }
  return count > 0;
}
static void test_addrinfo_contracts(void) {
  addrinfo_contract_get_fn get = (addrinfo_contract_get_fn)dlsym(RTLD_DEFAULT, "getaddrinfo");
  addrinfo_contract_network_fn network = (addrinfo_contract_network_fn)dlsym(RTLD_DEFAULT, "android_getaddrinfofornetwork");
  addrinfo_contract_free_fn release = (addrinfo_contract_free_fn)dlsym(RTLD_DEFAULT, "freeaddrinfo");
  check("getaddrinfo: public exports", get && network && release);
  if (!get || !network || !release) return;
  struct addrinfo_contract_row hints;
  memset(&hints, 0, sizeof(hints));
  hints.flags = 0x4 | 0x8;  // bionic AI_NUMERICHOST | AI_NUMERICSERV
  hints.family = AF_INET;
  hints.socktype = SOCK_STREAM;
  hints.protocol = IPPROTO_TCP;
  struct addrinfo_contract_row* result = NULL;
  int rc = get("127.0.0.1", "80", &hints, &result);
  check("getaddrinfo: numeric loopback retains address and protocol", rc == 0 && addrinfo_contract_numeric_list(result));
  release(result);
  result = NULL;
  rc = network(0, "127.0.0.1", "80", &hints, &result);
  check("getaddrinfo: default-network wrapper retains numeric results", rc == 0 && addrinfo_contract_numeric_list(result));
  release(result);
  release(NULL);
  check("getaddrinfo: null result pointer is a reported error", get("127.0.0.1", "80", &hints, NULL) == 4);
  result = NULL;
  errno = 0;
  rc = network(1, "127.0.0.1", "80", &hints, &result);
  check("getaddrinfo: invalid network handle remains EAI_SYSTEM/EINVAL", rc == 11 && errno == EINVAL);
}
