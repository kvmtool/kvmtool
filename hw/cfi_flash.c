#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "kvm/kvm.h"
#include "kvm/kvm-arch.h"
#include "kvm/kvm-cpu.h"
#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/mutex.h"
#include "kvm/util.h"

/*
 * The EDK2 driver hardcodes two 16-bit chips on a 32-bit bus.
 * This code supports one or two chips (enforced below).
 */
#define CFI_NR_FLASH_CHIPS			2

/* We always emulate a 32 bit bus width. */
#define CFI_BUS_WIDTH				4

/* The *effective* size of an erase block (over all chips) */
#define FLASH_BLOCK_SIZE			SZ_64K
#define FLASH_BLOCK_SIZE_PER_CHIP					\
	(FLASH_BLOCK_SIZE / CFI_NR_FLASH_CHIPS)

#define PROGRAM_BUFF_SIZE_BITS			7
#define PROGRAM_BUFF_SIZE			(1U << PROGRAM_BUFF_SIZE_BITS)
#define PROGRAM_BUFF_SIZE_BITS_PER_CHIP					\
	(PROGRAM_BUFF_SIZE_BITS + 1 - CFI_NR_FLASH_CHIPS)

/* CFI commands */
#define CFI_CMD_LOCK_BLOCK			0x01
#define CFI_CMD_ALTERNATE_WORD_PROGRAM		0x10
#define CFI_CMD_ERASE_BLOCK_SETUP		0x20
#define CFI_CMD_WORD_PROGRAM			0x40
#define CFI_CMD_CLEAR_STATUS_REG		0x50
#define CFI_CMD_LOCK_BLOCK_SETUP		0x60
#define CFI_CMD_READ_STATUS_REG			0x70
#define CFI_CMD_READ_JEDEC_DEVID		0x90
#define CFI_CMD_READ_CFI_QUERY			0x98
#define CFI_CMD_CONFIRM				0xd0
#define CFI_CMD_BUFFERED_PROGRAM_SETUP		0xe8
#define CFI_CMD_READ_ARRAY			0xff

#define CFI_STATUS_PROTECT_BIT		0x02
#define CFI_STATUS_PROGRAM_LOCK_BIT	0x10
#define CFI_STATUS_ERASE_CLEAR_LOCK_BIT	0x20
#define CFI_STATUS_LOCK_ERROR		CFI_STATUS_PROGRAM_LOCK_BIT |	\
					CFI_STATUS_PROTECT_BIT
#define CFI_STATUS_ERASE_ERROR		CFI_STATUS_ERASE_CLEAR_LOCK_BIT | \
					CFI_STATUS_PROGRAM_LOCK_BIT
#define CFI_STATUS_READY		0x80

/*
 * CFI query table contents, as far as it is constant.
 * The dynamic information (size, etc.) will be generated on the fly.
 */
#define CFI_GEOM_OFFSET				0x27
static const u8 cfi_query_table[] = {
		/* CFI query identification string */
	[0x10] = 'Q', 'R', 'Y',		/* ID string */
	0x01, 0x00,		/* primary command set: Intel/Sharp extended */
	0x31, 0x00,		/* address of primary extended query table */
	0x00, 0x00,		/* alternative command set: unused */
	0x00, 0x00,		/* address of alternative extended query table*/
		/* system interface information */
	[0x1b] = 0x45,			/* minimum Vcc voltage: 4.5V */
	0x55,			/* maximum Vcc voltage: 5.5V */
	0x00,			/* minimum Vpp voltage: 0.0V (unused) */
	0x00,			/* maximum Vpp voltage: 0.0V *(unused) */
	0x01,			/* timeout for single word program: 2 us */
	0x01,			/* timeout for multi-byte program: 2 us */
	0x01,			/* timeout for block erase: 2 ms */
	0x00,			/* timeout for full chip erase: not supported */
	0x00,			/* max timeout for single word program: 1x */
	0x00,			/* max timeout for mulit-byte program: 1x */
	0x00,			/* max timeout for block erase: 1x */
	0x00,			/* max timeout for chip erase: not supported */
		/* flash geometry information */
	[0x27] = 0x00,		/* size in power-of-2 bytes, filled later */
	0x05, 0x00,		/* interface description: 32 and 16 bits */
	PROGRAM_BUFF_SIZE_BITS_PER_CHIP, 0x00,
				/* number of bytes in write buffer */
	0x01,			/* one erase block region */
	0x00, 0x00, 0x00, 0x00, /* number and size of erase blocks, generated */
		/* Intel primary algorithm extended query table */
	[0x31] = 'P', 'R', 'I',
	'1', '0',		/* version 1.0 */
	0xa0, 0x00, 0x00, 0x00, /* optional features: instant lock & pm-read */
	0x00,			/* no functions after suspend */
	0x01, 0x00,		/* only lock bit supported */
	0x50,			/* best Vcc value: 5.0V */
	0x00,			/* best Vpp value: 0.0V (unused) */
	0x01,			/* number of protection register fields */
	0x00, 0x00, 0x00, 0x00,	/* protection field 1 description */
};

/*
 * Those states represent a subset of the CFI flash state machine.
 */
enum cfi_flash_state {
	READY,
	LOCK_BLOCK_SETUP,
	WORD_PROGRAM,
	BUFFERED_PROGRAM_SETUP,
	BUFFER_WRITE,
	ERASE_BLOCK_SETUP,
};

/*
 * The device can be in several **Read** modes.
 * We don't implement the asynchronous burst mode.
 */
enum cfi_read_mode {
	READ_ARRAY,
	READ_STATUS_REG,
	READ_JEDEC_DEVID,
	READ_CFI_QUERY,
};

struct cfi_flash_device {
	struct device_header	dev_hdr;
	/* Protects the CFI state machine variables in this data structure. */
	struct mutex		mutex;
	u64			base_addr;
	u32			size;

	void			*flash_memory;
	u8			program_buffer[PROGRAM_BUFF_SIZE];
	unsigned long		*lock_bm;
	u64			block_address;
	unsigned int		buff_written;
	unsigned int		buffer_length;

	enum cfi_flash_state	state;
	enum cfi_read_mode	read_mode;
	u8			sr;
	bool			is_mapped;
};

static int nr_erase_blocks(struct cfi_flash_device *sfdev)
{
	return sfdev->size / FLASH_BLOCK_SIZE;
}

/*
 * CFI queries always deal with one byte of information, possibly mirrored
 * to other bytes on the bus. This is dealt with in the callers.
 * The address provided is the one for 8-bit addressing, and would need to
 * be adjusted for wider accesses.
 */
static u8 read_cfi(struct cfi_flash_device *sfdev, u64 faddr)
{
	if (faddr > sizeof(cfi_query_table)) {
		pr_debug("CFI query read access beyond the end of table");
		return 0;
	}

	/* Fixup dynamic information in the geometry part of the table. */
	switch (faddr) {
	case 0x27:		/* device size in bytes, power of two */
		return pow2_size(sfdev->size / CFI_NR_FLASH_CHIPS);
	case 0x2d + 0:	/* number of erase blocks, minus one */
		return (nr_erase_blocks(sfdev) - 1) & 0xff;
	case 0x2d + 1:
		return ((nr_erase_blocks(sfdev) - 1) >> 8) & 0xff;
	case 0x2d + 2:	/* erase block size, in units of 256 */
		return (FLASH_BLOCK_SIZE_PER_CHIP / 256) & 0xff;
	case 0x2d + 3:
		return ((FLASH_BLOCK_SIZE_PER_CHIP / 256) >> 8) & 0xff;
	}

	return cfi_query_table[faddr];
}

static bool block_is_locked(struct cfi_flash_device *sfdev, u64 faddr)
{
	int block_nr = faddr / FLASH_BLOCK_SIZE;

	return test_bit(block_nr, sfdev->lock_bm);
}

#define DEV_ID_MASK 0x7ff
static u16 read_dev_id(struct cfi_flash_device *sfdev, u64 faddr)
{
	switch ((faddr & DEV_ID_MASK) / CFI_BUS_WIDTH) {
	case 0x0:				/* vendor ID */
		return 0x0000;
	case 0x1:				/* device ID */
		return 0xffff;
	case 0x2:
		return block_is_locked(sfdev, faddr & ~DEV_ID_MASK);
	default:			/* Ignore the other entries. */
		return 0;
	}
}

static void lock_block(struct cfi_flash_device *sfdev, u64 faddr, bool lock)
{
	int block_nr = faddr / FLASH_BLOCK_SIZE;

	if (lock)
		set_bit(block_nr, sfdev->lock_bm);
	else
		clear_bit(block_nr, sfdev->lock_bm);
}

static void word_program(struct cfi_flash_device *sfdev,
			 u64 faddr, void *data, int len)
{
	if (block_is_locked(sfdev, faddr)) {
		sfdev->sr |= CFI_STATUS_LOCK_ERROR;
		return;
	}

	memcpy(sfdev->flash_memory + faddr, data, len);
}

/* Reset the program buffer state to prepare for follow-up writes. */
static void buffer_setup(struct cfi_flash_device *sfdev)
{
	memset(sfdev->program_buffer, 0, sizeof(sfdev->program_buffer));
	sfdev->block_address = ~0ULL;
	sfdev->buff_written = 0;
}

static bool buffer_write(struct cfi_flash_device *sfdev,
			 u64 faddr, void *buffer, int len)
{
	unsigned int buff_addr;

	if (sfdev->buff_written >= sfdev->buffer_length)
		return false;

	/*
	 * The first word written into the buffer after the setup command
	 * happens to be the base address for the buffer.
	 * All subsequent writes need to be within this address and this
	 * address plus the buffer size, so keep this value around.
	 */
	if (sfdev->block_address == ~0ULL)
		sfdev->block_address = faddr;

	if (faddr < sfdev->block_address)
		return false;
	buff_addr = faddr - sfdev->block_address;
	if (buff_addr >= PROGRAM_BUFF_SIZE)
		return false;

	memcpy(sfdev->program_buffer + buff_addr, buffer, len);
	sfdev->buff_written += len;

	return true;
}

static void buffer_confirm(struct cfi_flash_device *sfdev)
{
	if (block_is_locked(sfdev, sfdev->block_address)) {
		sfdev->sr |= CFI_STATUS_LOCK_ERROR;
		return;
	}
	memcpy(sfdev->flash_memory + sfdev->block_address,
	       sfdev->program_buffer, sfdev->buff_written);
}

static void block_erase_confirm(struct cfi_flash_device *sfdev, u64 faddr)
{
	if (block_is_locked(sfdev, faddr)) {
		sfdev->sr |= CFI_STATUS_LOCK_ERROR;
		return;
	}

	memset(sfdev->flash_memory + faddr, 0xff, FLASH_BLOCK_SIZE);
}

static void cfi_flash_read(struct cfi_flash_device *sfdev,
			   u64 faddr, u8 *data, u32 len)
{
	u16 cfi_value = 0;

	switch (sfdev->read_mode) {
	case READ_ARRAY:
		/* just copy the requested bytes from the array */
		memcpy(data, sfdev->flash_memory + faddr, len);
		return;
	case READ_STATUS_REG:
		cfi_value = sfdev->sr;
		break;
	case READ_JEDEC_DEVID:
		cfi_value = read_dev_id(sfdev, faddr);
		break;
	case READ_CFI_QUERY:
		cfi_value = read_cfi(sfdev, faddr / CFI_BUS_WIDTH);
		break;
	}
	switch (len) {
	case 1:
		*data = cfi_value;
		break;
	case 8: memset(data + 4, 0, 4);
		/* fall-through */
	case 4:
		if (CFI_NR_FLASH_CHIPS == 2)
			memcpy(data + 2, &cfi_value, 2);
		else
			memset(data + 2, 0, 2);
		/* fall-through */
	case 2:
		memcpy(data, &cfi_value, 2);
		break;
	default:
		pr_debug("CFI flash: illegal access length %d for read mode %d",
			 len, sfdev->read_mode);
		break;
	}
}

/*
 * Any writes happening in "READY" state don't actually write to the memory,
 * but are really treated as commands to advance the state machine and select
 * the next action.
 * Change the state and modes according to the value written. The address
 * that value is written to does not matter and is ignored.
 */
static void cfi_flash_write_ready(struct cfi_flash_device *sfdev, u8 command)
{
	switch (command) {
	case CFI_CMD_READ_JEDEC_DEVID:
		sfdev->read_mode = READ_JEDEC_DEVID;
		break;
	case CFI_CMD_READ_STATUS_REG:
		sfdev->read_mode = READ_STATUS_REG;
		break;
	case CFI_CMD_READ_CFI_QUERY:
		sfdev->read_mode = READ_CFI_QUERY;
		break;
	case CFI_CMD_CLEAR_STATUS_REG:
		sfdev->sr = CFI_STATUS_READY;
		break;
	case CFI_CMD_WORD_PROGRAM:
	case CFI_CMD_ALTERNATE_WORD_PROGRAM:
		sfdev->state = WORD_PROGRAM;
		sfdev->read_mode = READ_STATUS_REG;
		break;
	case CFI_CMD_LOCK_BLOCK_SETUP:
		sfdev->state = LOCK_BLOCK_SETUP;
		break;
	case CFI_CMD_ERASE_BLOCK_SETUP:
		sfdev->state = ERASE_BLOCK_SETUP;
		sfdev->read_mode = READ_STATUS_REG;
		break;
	case CFI_CMD_BUFFERED_PROGRAM_SETUP:
		buffer_setup(sfdev);
		sfdev->state = BUFFERED_PROGRAM_SETUP;
		sfdev->read_mode = READ_STATUS_REG;
		break;
	case CFI_CMD_CONFIRM:
		pr_debug("CFI flash: unexpected confirm command 0xd0");
		break;
	default:
		pr_debug("CFI flash: unknown command 0x%x", command);
		/* fall-through */
	case CFI_CMD_READ_ARRAY:
		sfdev->read_mode = READ_ARRAY;
		break;
	}
}

static void cfi_flash_write(struct cfi_flash_device *sfdev, u16 command,
			    u64 faddr, u8 *data, u32 len)
{
	switch (sfdev->state) {
	case READY:
		cfi_flash_write_ready(sfdev, command & 0xff);
		return;
	case LOCK_BLOCK_SETUP:
		switch (command & 0xff) {
		case CFI_CMD_LOCK_BLOCK:
			lock_block(sfdev, faddr, true);
			sfdev->read_mode = READ_STATUS_REG;
			break;
		case CFI_CMD_CONFIRM:
			lock_block(sfdev, faddr, false);
			sfdev->read_mode = READ_STATUS_REG;
			break;
		default:
			sfdev->sr |= CFI_STATUS_ERASE_ERROR;
			break;
		}
		sfdev->state = READY;
		break;

	case WORD_PROGRAM:
		word_program(sfdev, faddr, data, len);
		sfdev->read_mode = READ_STATUS_REG;
		sfdev->state = READY;
		break;

	case BUFFER_WRITE:
		if (buffer_write(sfdev, faddr, data, len))
			break;

		if ((command & 0xff) == CFI_CMD_CONFIRM) {
			buffer_confirm(sfdev);
			sfdev->read_mode = READ_STATUS_REG;
		} else {
			pr_debug("CFI flash: BUFFER_WRITE: expected CONFIRM(0xd0), got 0x%x @ 0x%llx",
				 command, faddr);
			sfdev->sr |= CFI_STATUS_PROGRAM_LOCK_BIT;
		}
		sfdev->state = READY;
		break;

	case BUFFERED_PROGRAM_SETUP:
		sfdev->buffer_length = (command + 1) * CFI_BUS_WIDTH;
		if (sfdev->buffer_length > PROGRAM_BUFF_SIZE)
			sfdev->buffer_length = PROGRAM_BUFF_SIZE;
		sfdev->state = BUFFER_WRITE;
		sfdev->read_mode = READ_STATUS_REG;
		break;

	case ERASE_BLOCK_SETUP:
		if ((command & 0xff) == CFI_CMD_CONFIRM)
			block_erase_confirm(sfdev, faddr);
		else
			sfdev->sr |= CFI_STATUS_ERASE_ERROR;

		sfdev->state = READY;
		sfdev->read_mode = READ_STATUS_REG;
		break;
	default:
		pr_debug("CFI flash: unexpected/unknown command 0x%x", command);
		break;
	}
}

/*
 * If we are in ARRAY_READ mode, we can map the flash array directly
 * into the guest, just as read-only. This greatly improves read
 * performance, and avoids problems with exits due to accesses from
 * load instructions without syndrome information (on ARM).
 * Also it could allow code to be executed XIP in there.
 */
static int map_flash_memory(struct kvm *kvm, struct cfi_flash_device *sfdev)
{
	int ret;

	ret = kvm__register_mem(kvm, sfdev->base_addr, sfdev->size,
				sfdev->flash_memory,
				KVM_MEM_TYPE_RAM | KVM_MEM_TYPE_READONLY);
	if (!ret)
		sfdev->is_mapped = true;

	return ret;
}

/*
 * Any write access changing the read mode would need to bring us back to
 * "trap everything", as the CFI query read need proper handholding.
 */
static int unmap_flash_memory(struct kvm *kvm, struct cfi_flash_device *sfdev)
{
	int ret;

	ret = kvm__destroy_mem(kvm, sfdev->base_addr, sfdev->size,
			       sfdev->flash_memory);

	if (!ret)
		sfdev->is_mapped = false;

	return ret;
}

static void cfi_flash_mmio(struct kvm_cpu *vcpu,
			   u64 addr, u8 *data, u32 len, u8 is_write,
			   void *context)
{
	struct cfi_flash_device *sfdev = context;
	u64 faddr = addr - sfdev->base_addr;
	u32 value;

	if (!is_write) {
		mutex_lock(&sfdev->mutex);

		cfi_flash_read(sfdev, faddr, data, len);

		mutex_unlock(&sfdev->mutex);

		return;
	}

	if (len > 4) {
		pr_info("CFI flash: MMIO %d-bit write access not supported",
			 len * 8);
		return;
	}

	memcpy(&value, data, len);

	mutex_lock(&sfdev->mutex);

	cfi_flash_write(sfdev, value & 0xffff, faddr, data, len);

	/* Adjust our mapping status accordingly. */
	if (!sfdev->is_mapped && sfdev->read_mode == READ_ARRAY)
		map_flash_memory(vcpu->kvm, sfdev);
	else if (sfdev->is_mapped && sfdev->read_mode != READ_ARRAY)
		unmap_flash_memory(vcpu->kvm, sfdev);

	mutex_unlock(&sfdev->mutex);
}

#ifdef CONFIG_HAS_LIBFDT
static void generate_cfi_flash_fdt_node(void *fdt,
					struct device_header *dev_hdr,
					void (*generate_irq_prop)(void *fdt,
								  u8 irq,
								enum irq_type))
{
	struct cfi_flash_device *sfdev;
	u64 reg_prop[2];

	sfdev = container_of(dev_hdr, struct cfi_flash_device, dev_hdr);
	reg_prop[0] = cpu_to_fdt64(sfdev->base_addr);
	reg_prop[1] = cpu_to_fdt64(sfdev->size);

	_FDT(fdt_begin_node(fdt, "flash"));
	_FDT(fdt_property_cell(fdt, "bank-width", CFI_BUS_WIDTH));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x1));
	_FDT(fdt_property_string(fdt, "compatible", "cfi-flash"));
	_FDT(fdt_property_string(fdt, "label", "System-firmware"));
	_FDT(fdt_property(fdt, "reg", &reg_prop, sizeof(reg_prop)));
	_FDT(fdt_end_node(fdt));
}
#else
#define generate_cfi_flash_fdt_node NULL
#endif

static struct cfi_flash_device *create_flash_device_file(struct kvm *kvm,
							 const char *filename)
{
	struct cfi_flash_device *sfdev;
	struct stat statbuf;
	unsigned int value;
	int ret;
	int fd;

	fd = open(filename, O_RDWR);
	if (fd < 0)
		return ERR_PTR(-errno);

	if (fstat(fd, &statbuf) < 0) {
		ret = -errno;
		goto out_close;
	}

	sfdev = malloc(sizeof(struct cfi_flash_device));
	if (!sfdev) {
		ret = -ENOMEM;
		goto out_close;
	}

	sfdev->size = statbuf.st_size;
	/* Round down to nearest power-of-2 size value. */
	sfdev->size = 1U << (pow2_size(sfdev->size + 1) - 1);
	if (sfdev->size > KVM_FLASH_MAX_SIZE)
		sfdev->size = KVM_FLASH_MAX_SIZE;
	if (sfdev->size < statbuf.st_size) {
		pr_info("flash file size (%llu bytes) is not a power of two",
			(unsigned long long)statbuf.st_size);
		pr_info("only using first %u bytes", sfdev->size);
	}
	sfdev->flash_memory = mmap(NULL, sfdev->size,
				   PROT_READ | PROT_WRITE, MAP_SHARED,
				   fd, 0);
	if (sfdev->flash_memory == MAP_FAILED) {
		ret = -errno;
		goto out_free;
	}
	sfdev->base_addr = KVM_FLASH_MMIO_BASE;
	sfdev->state = READY;
	sfdev->read_mode = READ_ARRAY;
	sfdev->sr = CFI_STATUS_READY;

	map_flash_memory(kvm, sfdev);

	value = roundup(nr_erase_blocks(sfdev), BITS_PER_LONG) / 8;
	sfdev->lock_bm = malloc(value);
	memset(sfdev->lock_bm, 0, value);

	sfdev->dev_hdr.bus_type = DEVICE_BUS_MMIO;
	sfdev->dev_hdr.data = generate_cfi_flash_fdt_node;
	mutex_init(&sfdev->mutex);
	ret = device__register(&sfdev->dev_hdr);
	if (ret)
		goto out_unmap;

	ret = kvm__register_mmio(kvm,
				 sfdev->base_addr, sfdev->size,
				 false, cfi_flash_mmio, sfdev);
	if (ret) {
		device__unregister(&sfdev->dev_hdr);
		goto out_unmap;
	}

	return sfdev;

out_unmap:
	munmap(sfdev->flash_memory, sfdev->size);
out_free:
	free(sfdev);
out_close:
	close(fd);

	return ERR_PTR(ret);
}

static int cfi_flash__init(struct kvm *kvm)
{
	struct cfi_flash_device *sfdev;

	BUILD_BUG_ON(CFI_NR_FLASH_CHIPS != 1 && CFI_NR_FLASH_CHIPS != 2);

	if (!kvm->cfg.flash_filename)
		return 0;

	sfdev = create_flash_device_file(kvm, kvm->cfg.flash_filename);
	if (IS_ERR(sfdev))
		return PTR_ERR(sfdev);

	return 0;
}
dev_init(cfi_flash__init);
