#ifndef BIOS_EXPORT_H_
#define BIOS_EXPORT_H_

struct kvm;

extern char bios_intfake[0];
extern char bios_intfake_end[0];

extern char bios_int10[0];
extern char bios_int10_end[0];

extern char bios_int15[0];
extern char bios_int15_end[0];

#define bios_intfake_size	(bios_intfake_end - bios_intfake)
#define bios_int10_size		(bios_int10_end - bios_int10)
#define bios_int15_size		(bios_int15_end - bios_int15)

extern void setup_bios(struct kvm *kvm);

#endif /* BIOS_EXPORT_H_ */
