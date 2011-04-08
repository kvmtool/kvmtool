#ifndef KVM_READ_WRITE_H
#define KVM_READ_WRITE_H

#include <sys/types.h>
#include <unistd.h>

ssize_t xread(int fd, void *buf, size_t count);
ssize_t xwrite(int fd, const void *buf, size_t count);

ssize_t read_in_full(int fd, void *buf, size_t count);
ssize_t write_in_full(int fd, const void *buf, size_t count);

ssize_t xpread(int fd, void *buf, size_t count, off_t offset);
ssize_t xpwrite(int fd, const void *buf, size_t count, off_t offset);

ssize_t pread_in_full(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite_in_full(int fd, const void *buf, size_t count, off_t offset);

#endif /* KVM_READ_WRITE_H */
