#ifndef KVM__RTC_H
#define KVM__RTC_H

struct kvm;

int rtc__init(struct kvm *kvm);
int rtc__exit(struct kvm *kvm);

#endif /* KVM__RTC_H */
