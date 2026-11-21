// Forced include on Switch and PS4 (CMakeLists.txt): what their SDK headers miss
#pragma once

#if defined(__PS4__)

#include <signal.h>
#include <sys/ioctl.h>

// FreeBSD values (sys/filio.h): the OpenOrbis musl headers keep only the Linux ones, as LINUX_*
#ifndef FIONREAD
#define FIONREAD _IOR('f', 127, int)
#endif
#ifndef FIONBIO
#define FIONBIO _IOW('f', 126, int)
#endif

// the union member is __sa_sigaction, the macro points to sa_sigaction
#undef sa_sigaction
#define sa_sigaction __sa_handler.__sa_sigaction

#endif

#if defined(__SWITCH__)

#include <errno.h>
#include <stddef.h>
#include <netinet/in.h>
#include <net/if.h>

// newlib declares it only with the Linux errno extensions
#ifndef ESHUTDOWN
#define ESHUTDOWN 110
#endif

// no IPv6 in the Switch BSD service: what asio names but never reaches over IPv4
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};

#ifdef __cplusplus
extern "C" {
#endif

// commented out in libnx net/if.h
static inline unsigned int if_nametoindex(const char *name) {
    (void) name;
    errno = ENXIO;
    return 0;
}

static inline char *if_indextoname(unsigned int index, char *name) {
    (void) index;
    (void) name;
    errno = ENXIO;
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif
