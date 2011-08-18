#include "kvm/rtc.h"

#include "kvm/ioport.h"
#include "kvm/kvm.h"

#include <time.h>

static u8 cmos_index;

#define CMOS_RTC_SECONDS		0x00
#define CMOS_RTC_MINUTES		0x02
#define CMOS_RTC_HOURS			0x04
#define CMOS_RTC_DATE_OF_MONTH		0x07
#define CMOS_RTC_MONTH			0x08
#define CMOS_RTC_YEAR			0x09

static inline unsigned char bin2bcd(unsigned val)
{
	return ((val / 10) << 4) + val % 10;
}

static bool cmos_ram_data_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	struct tm *tm;
	time_t ti;

	time(&ti);

	tm = gmtime(&ti);

	switch (cmos_index) {
	case CMOS_RTC_SECONDS:
		ioport__write8(data, bin2bcd(tm->tm_sec));
		break;
	case CMOS_RTC_MINUTES:
		ioport__write8(data, bin2bcd(tm->tm_min));
		break;
	case CMOS_RTC_HOURS:
		ioport__write8(data, bin2bcd(tm->tm_hour));
		break;
	case CMOS_RTC_DATE_OF_MONTH:
		ioport__write8(data, bin2bcd(tm->tm_mday));
		break;
	case CMOS_RTC_MONTH:
		ioport__write8(data, bin2bcd(tm->tm_mon + 1));
		break;
	case CMOS_RTC_YEAR:
		ioport__write8(data, bin2bcd(tm->tm_year));
		break;
	}

	return true;
}

static bool cmos_ram_data_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return true;
}

static struct ioport_operations cmos_ram_data_ioport_ops = {
	.io_out		= cmos_ram_data_out,
	.io_in		= cmos_ram_data_in,
};

static bool cmos_ram_index_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	u8 value;

	value	= ioport__read8(data);

	kvm->nmi_disabled	= value & (1UL << 7);

	cmos_index		= value & ~(1UL << 7);

	return true;
}

static struct ioport_operations cmos_ram_index_ioport_ops = {
	.io_out		= cmos_ram_index_out,
};

void rtc__init(void)
{
	/* PORT 0070-007F - CMOS RAM/RTC (REAL TIME CLOCK) */
	ioport__register(0x0070, &cmos_ram_index_ioport_ops, 1, NULL);
	ioport__register(0x0071, &cmos_ram_data_ioport_ops, 1, NULL);
}
