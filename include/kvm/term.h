#ifndef KVM__TERM_H
#define KVM__TERM_H

#include "kvm/kvm.h"

#include <sys/uio.h>
#include <stdbool.h>

#define CONSOLE_8250	1
#define CONSOLE_VIRTIO	2
#define CONSOLE_HV	3

int term_putc_iov(struct iovec *iov, int iovcnt, int term);
int term_getc_iov(struct kvm *kvm, struct iovec *iov, int iovcnt, int term);
int term_putc(char *addr, int cnt, int term);
int term_getc(struct kvm *kvm, int term);

bool term_readable(int term);
void term_set_tty(int term);
int term_init(struct kvm *kvm);
int term_exit(struct kvm *kvm);
int tty_parser(const struct option *opt, const char *arg, int unset);

#endif /* KVM__TERM_H */
