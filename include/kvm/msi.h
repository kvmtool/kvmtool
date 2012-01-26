#ifndef LKVM_MSI_H
#define LKVM_MSI_H

struct msi_msg {
	u32	address_lo;	/* low 32 bits of msi message address */
	u32	address_hi;	/* high 32 bits of msi message address */
	u32	data;		/* 16 bits of msi message data */
};

#endif /* LKVM_MSI_H */
