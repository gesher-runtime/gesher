// gesher-base: Android's INTERNET permission (docs/network.md).
//
// On Android an app without android.permission.INTERNET runs without the inet
// group: creating an AF_INET or AF_INET6 socket fails with EACCES, and so does
// a name lookup (EAI_SYSTEM with errno EACCES, which libcore reports as
// "Permission denied (missing INTERNET permission?)"). Gesher's app processes
// get the same: GESHER_INET=0 in the environment (the system server sets it
// for packages that do not request INTERNET), or gesher_set_inet_allowed(0).
// Loopback socketpairs and AF_UNIX sockets are not affected.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gesher_set_inet_allowed(int allowed);
int gesher_inet_allowed(void);

// Interface 0 clears a binding. These return 0 or a positive Linux errno;
// no Windows/CRT errno is read across the boundary.
int gesher_bind_socket_interface(int fd, uint32_t if_index);
int gesher_socket_interface(int fd, uint32_t* if_index);
int gesher_set_process_interface(uint32_t if_index);
uint32_t gesher_process_interface(void);
// A newline-separated list of numeric addresses, allocated with malloc.
// An explicit interface never falls back to a different network.
int gesher_resolve_interface(const char* host, uint32_t if_index, char** addresses);
struct addrinfo;
int gesher_getaddrinfo_interface(const char* host, const char* service,
    const struct addrinfo* hints, uint32_t if_index, struct addrinfo** addresses);

// Android's raw asynchronous resolver: a pollable descriptor, a packet result
// (the descriptor is consumed), or cancellation. Failures are negative errno.
int gesher_res_nsend(uint32_t if_index, const uint8_t* query, size_t length, uint32_t flags);
int gesher_res_nquery(uint32_t if_index, const char* name, int dns_class, int type, uint32_t flags);
int gesher_res_nresult(int fd, int* rcode, uint8_t* answer, size_t capacity);
void gesher_res_cancel(int fd);

#ifdef __cplusplus
}
#endif
