// libc functions newlib on the Switch does not provide, needed by asio and libtorrent
#ifdef __SWITCH__

#include <errno.h>
#include <unistd.h>

#include <switch.h>

// asio names it in null_event (the lock-free scheduler mode, unused here)
int pause(void) {
    for (;;) {
        svcSleepThread(1000000000LL);
    }
    return -1;
}

// not atomic: seek, read or write, seek back; libtorrent uses them on its own descriptors
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, count);
    int err = errno;
    lseek(fd, cur, SEEK_SET);
    errno = err;
    return n;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t n = write(fd, buf, count);
    int err = errno;
    lseek(fd, cur, SEEK_SET);
    errno = err;
    return n;
}

#endif
