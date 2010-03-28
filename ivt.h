#include <inttypes.h>
#include <stdbool.h>

#define IVT_BASE	0x0000
#define IVT_VECTORS	256

struct ivt_entry {
	uint16_t segment;
	uint16_t offset;
} __attribute__((packed));

void ivt_reset(void);
void ivt_copy_table(void *dst, unsigned int size);
struct ivt_entry * const ivt_get_entry(unsigned int n);
void ivt_set_entry(struct ivt_entry e, unsigned int n);
