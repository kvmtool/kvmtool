#ifndef BIOS_EXPORT_H_
#define BIOS_EXPORT_H_

struct kvm;

extern char bios_rom[0];
extern char bios_rom_end[0];

#define bios_rom_size		(bios_rom_end - bios_rom)

extern void setup_bios(struct kvm *kvm);

#endif /* BIOS_EXPORT_H_ */
