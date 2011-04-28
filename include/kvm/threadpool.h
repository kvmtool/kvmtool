#ifndef KVM__THREADPOOL_H
#define KVM__THREADPOOL_H

#include <stdint.h>

struct kvm;

typedef void (*kvm_thread_callback_fn_t)(struct kvm *kvm, void *data);

int thread_pool__init(unsigned long thread_count);

void *thread_pool__add_jobtype(struct kvm *kvm, kvm_thread_callback_fn_t callback, void *data);

void thread_pool__signal_work(void *job);

#endif
