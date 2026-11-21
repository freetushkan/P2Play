// devkitA64 newlib has no <sys/uio.h> (devkitPro/newlib#18), libnx defines struct iovec
#pragma once

#include <limits.h>
#include <sys/_iovec.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef IOV_MAX
#define IOV_MAX 64
#endif

#ifdef __cplusplus
extern "C" {
#endif

// not atomic, a loop over read/write
static inline ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((size_t) n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

static inline ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((size_t) n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

#ifdef __cplusplus
}
#endif
