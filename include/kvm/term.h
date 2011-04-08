#ifndef KVM__TERM_H
#define KVM__TERM_H

#include <sys/uio.h>

#define CONSOLE_8250	1
#define CONSOLE_VIRTIO	2

int term_putc_iov(int who, struct iovec *iov, int iovcnt);
int term_getc_iov(int who, struct iovec *iov, int iovcnt);
int term_putc(int who, char *addr, int cnt);
int term_getc(int who);

bool term_readable(int who);
void term_init(void);

#endif /* KVM__TERM_H */
