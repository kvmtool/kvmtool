#include "kvm/kvm-cpu.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"
#include "kvm/term.h"

#include <asm/ptrace.h>

static int debug_fd;

void kvm_cpu__set_debug_fd(int fd)
{
	debug_fd = fd;
}

int kvm_cpu__get_debug_fd(void)
{
	return debug_fd;
}

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id)
{
	/* TODO: */
	return NULL;
}

void kvm_cpu__arch_nmi(struct kvm_cpu *cpu)
{
}

void kvm_cpu__delete(struct kvm_cpu *vcpu)
{
	/* TODO: */
}

bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu)
{
	/* TODO: */
	return false;
}

void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu)
{
	/* TODO: */
}

void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu)
{
	/* TODO: */
}

int kvm_cpu__get_endianness(struct kvm_cpu *vcpu)
{
	return VIRTIO_ENDIAN_LE;
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu)
{
	/* TODO: */
}

void kvm_cpu__show_registers(struct kvm_cpu *vcpu)
{
	/* TODO: */
}
