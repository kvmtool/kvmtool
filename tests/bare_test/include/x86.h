#ifndef X86_H
#define X86_H
#include <stddef.h>

static inline uint32_t readeflags(void) {
    uint64_t eflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(eflags));
    return (uint32_t)eflags;
}

static inline void reboot(void) {
    __asm__ volatile(
        "mov	$0xfe, %al\n\t"
        "outb	%al, $0x64\n\t"
    );
}

#endif