// Real NDK libandroid networking calls, through the loaded ELF/System V ABI.
#include <android/multinetwork.h>
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__aarch64__)
#define WEB3_ABI "arm64"
#elif defined(__x86_64__)
#define WEB3_ABI "x86_64"
#else
#error Unsupported web3 test architecture
#endif

static int connected_source(net_handle_t network, const char* host, int port, int explicit) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  inet_pton(AF_INET, host, &destination.sin_addr);
  int ok = !explicit || android_setsocknetwork(network, fd) == 0;
  if (ok) ok = connect(fd, (struct sockaddr*)&destination, sizeof(destination)) == 0;
  struct sockaddr_in source;
  socklen_t length = sizeof(source);
  if (ok) ok = getsockname(fd, (struct sockaddr*)&source, &length) == 0 &&
      source.sin_addr.s_addr == destination.sin_addr.s_addr;
  close(fd);
  return ok;
}

// These NDK calls consume the query descriptor. Retire our local ownership
// immediately; another app thread may reuse its number before the call returns.
static int consume_result(int* fd, int* rcode, uint8_t* answer, size_t capacity) {
  int size = android_res_nresult(*fd, rcode, answer, capacity);
  *fd = -1;
  return size;
}

static void cancel_query(int* fd) {
  if (*fd >= 0) {
    android_res_cancel(*fd);
    *fd = -1;
  }
}

static int result(int fd, int expected_rcode, int expected_id) {
  if (fd < 0) {
    printf("Web3 DNS query failed: result=%d errno=%d\n", fd, errno);
    return 0;
  }
  struct pollfd pending = {.fd = fd, .events = POLLIN};
  int ready = poll(&pending, 1, 15000);
  if (ready != 1) {
    printf("Web3 DNS poll failed: result=%d revents=%d errno=%d\n", ready, pending.revents, errno);
    cancel_query(&fd);
    return 0;
  }
  uint8_t packet[65535];
  int rcode = -1;
  int query_fd = fd;  // Diagnostic value only, never used for a later syscall.
  int size = consume_result(&fd, &rcode, packet, sizeof(packet));
  int ok = size >= 12 && rcode == expected_rcode && (packet[2] & 0x80) &&
      (expected_id < 0 || ((int)packet[0] << 8 | packet[1]) == expected_id);
  if (!ok) printf("Web3 DNS result failed: fd=%d size=%d rcode=%d expected_rcode=%d expected_id=%d revents=%d packet_flags=%d errno=%d\n",
                  query_fd, size, rcode, expected_rcode, expected_id, pending.revents,
                  size >= 3 ? packet[2] : -1, errno);
  return ok;
}

JNIEXPORT jstring JNICALL Java_com_gesher_web3app_NetActivity_nativeNetworkCheck(
    JNIEnv* env, jclass unused, jlong handle, jstring address, jint port) {
  (void)unused;
  const char* host = (*env)->GetStringUTFChars(env, address, NULL);
  if (host == NULL) return NULL;
  const char* stage = "socket binding/source";
  net_handle_t network = (net_handle_t)handle;
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *addresses = NULL;
  int fd = -1;
  int failure_errno = 0;
  int ok = connected_source(network, host, port, 1);
  if (!ok) goto done;
  stage = "process binding/source";
  ok = android_setprocnetwork(network) == 0 && connected_source(network, host, port, 0);
  if (!ok) goto done;
  stage = "process binding clear";
  ok = android_setprocnetwork(NETWORK_UNSPECIFIED) == 0;
  if (!ok) goto done;
  stage = "per-network getaddrinfo";
  ok = android_getaddrinfofornetwork(network, "example.com", "443", &hints, &addresses) == 0 && addresses;
  if (addresses) { freeaddrinfo(addresses); addresses = NULL; }
  if (!ok) goto done;
  stage = "invalid handle error";
  errno = 0;
  ok = android_setprocnetwork(1) == -1 && errno == EINVAL;
  if (!ok) goto done;
  stage = "missing network refuses DNS fallback";
  net_handle_t missing = (UINT64_C(0x7fffffff) << 32) | UINT64_C(0xcafed00d);
  ok = android_getaddrinfofornetwork(missing, "example.com", NULL, &hints, &addresses) == EAI_SYSTEM;
  if (addresses) { freeaddrinfo(addresses); addresses = NULL; }
  if (!ok) goto done;
  stage = "pollable DNS query/result";
  ok = result(android_res_nquery(network, "example.com", 1, 1,
              ANDROID_RESOLV_NO_CACHE_LOOKUP), 0, -1);
  if (!ok) goto done;
  stage = "raw DNS packet/result";
  const uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,
                          7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
  ok = result(android_res_nsend(network, query, sizeof(query), ANDROID_RESOLV_NO_CACHE_STORE), 0, 0x1234);
  if (!ok) goto done;
  stage = "raw DNS NXDOMAIN";
  ok = result(android_res_nquery(network, "gesher-web3-no-such-name.invalid", 1, 1, 0), 3, -1);
  if (!ok) goto done;
  stage = "small DNS result buffer preserves rcode";
  fd = android_res_nquery(network, "example.com", 1, 1, 0);
  ok = fd >= 0;
  if (!ok) goto done;
  struct pollfd short_result = {.fd = fd, .events = POLLIN};
  int rcode = 77;
  uint8_t small[1];
  ok = poll(&short_result, 1, 15000) == 1;
  if (!ok) goto done;  // The query remains owned and must be cancelled below.
  ok = consume_result(&fd, &rcode, small, sizeof(small)) == -EMSGSIZE && rcode == 77;
  if (!ok) goto done;
  stage = "DNS cancellation";
  fd = android_res_nquery(network, "example.com", 1, 1, 0);
  ok = fd >= 0;
  if (!ok) goto done;
  cancel_query(&fd);
  stage = "invalid query flags/network";
  ok = android_res_nquery(network, "example.com", 1, 1, 8) == -EINVAL &&
      android_res_nquery(missing, "example.com", 1, 1, 0) == -ENONET;
done:
  failure_errno = errno;
  cancel_query(&fd);
  android_setprocnetwork(NETWORK_UNSPECIFIED);
  (*env)->ReleaseStringUTFChars(env, address, host);
  char message[256];
  if (ok) snprintf(message, sizeof(message), "PASS selected-network sockets/process/DNS, raw DNS poll/result/NXDOMAIN/cancel/errors abi=" WEB3_ABI);
  else snprintf(message, sizeof(message), "FAIL %s (errno %d)", stage, failure_errno);
  return (*env)->NewStringUTF(env, message);
}
