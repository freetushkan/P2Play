// devkitA64 has no AF_UNIX: asio includes this header unconditionally
// (local sockets are off with BOOST_ASIO_DISABLE_LOCAL_SOCKETS)
#pragma once

#include <sys/socket.h>

struct sockaddr_un {
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};
