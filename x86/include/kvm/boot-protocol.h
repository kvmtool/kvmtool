/*
 * Linux boot protocol specifics
 */

#ifndef BOOT_PROTOCOL_H_
#define BOOT_PROTOCOL_H_

/*
 * The protected mode kernel part of a modern bzImage is loaded
 * at 1 MB by default.
 */
#define BZ_DEFAULT_SETUP_SECTS		4
#define BZ_KERNEL_START			0x100000UL
#define INITRD_START			0x1000000UL

#endif /* BOOT_PROTOCOL_H_ */
