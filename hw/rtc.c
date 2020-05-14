#include "kvm/rtc.h"

#include "kvm/ioport.h"
#include "kvm/kvm.h"

#include <time.h>

/*
 * MC146818 RTC registers
 */
#define RTC_SECONDS			0x00
#define RTC_SECONDS_ALARM		0x01
#define RTC_MINUTES			0x02
#define RTC_MINUTES_ALARM		0x03
#define RTC_HOURS			0x04
#define RTC_HOURS_ALARM			0x05
#define RTC_DAY_OF_WEEK			0x06
#define RTC_DAY_OF_MONTH		0x07
#define RTC_MONTH			0x08
#define RTC_YEAR			0x09
#define RTC_CENTURY			0x32

#define RTC_REG_A			0x0A
#define RTC_REG_B			0x0B
#define RTC_REG_C			0x0C
#define RTC_REG_D			0x0D

/*
 * Register D Bits
 */
#define RTC_REG_D_VRT			(1 << 7)

struct rtc_device {
	u8			cmos_idx;
	u8			cmos_data[128];
};

static struct rtc_device	rtc;

static inline unsigned char bin2bcd(unsigned val)
{
	return ((val / 10) << 4) + val % 10;
}

static bool cmos_ram_data_in(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	struct tm *tm;
	time_t ti;

	time(&ti);

	tm = gmtime(&ti);

	switch (rtc.cmos_idx) {
	case RTC_SECONDS:
		ioport__write8(data, bin2bcd(tm->tm_sec));
		break;
	case RTC_MINUTES:
		ioport__write8(data, bin2bcd(tm->tm_min));
		break;
	case RTC_HOURS:
		ioport__write8(data, bin2bcd(tm->tm_hour));
		break;
	case RTC_DAY_OF_WEEK:
		ioport__write8(data, bin2bcd(tm->tm_wday + 1));
		break;
	case RTC_DAY_OF_MONTH:
		ioport__write8(data, bin2bcd(tm->tm_mday));
		break;
	case RTC_MONTH:
		ioport__write8(data, bin2bcd(tm->tm_mon + 1));
		break;
	case RTC_YEAR: {
		int year;

		year = tm->tm_year + 1900;

		ioport__write8(data, bin2bcd(year % 100));

		break;
	}
	case RTC_CENTURY: {
		int year;

		year = tm->tm_year + 1900;

		ioport__write8(data, bin2bcd(year / 100));

		break;
	}
	default:
		ioport__write8(data, rtc.cmos_data[rtc.cmos_idx]);
		break;
	}

	return true;
}

static bool cmos_ram_data_out(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	switch (rtc.cmos_idx) {
	case RTC_REG_C:
	case RTC_REG_D:
		/* Read-only */
		break;
	default:
		rtc.cmos_data[rtc.cmos_idx] = ioport__read8(data);
		break;
	}

	return true;
}

static struct ioport_operations cmos_ram_data_ioport_ops = {
	.io_out		= cmos_ram_data_out,
	.io_in		= cmos_ram_data_in,
};

static bool cmos_ram_index_out(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	u8 value = ioport__read8(data);

	vcpu->kvm->nmi_disabled	= value & (1UL << 7);
	rtc.cmos_idx		= value & ~(1UL << 7);

	return true;
}

static struct ioport_operations cmos_ram_index_ioport_ops = {
	.io_out		= cmos_ram_index_out,
};

#ifdef CONFIG_HAS_LIBFDT
static void generate_rtc_fdt_node(void *fdt,
				  struct device_header *dev_hdr,
				  void (*generate_irq_prop)(void *fdt,
							    u8 irq,
							    enum irq_type))
{
	u64 reg_prop[2] = { cpu_to_fdt64(0x70), cpu_to_fdt64(2) };

	_FDT(fdt_begin_node(fdt, "rtc"));
	_FDT(fdt_property_string(fdt, "compatible", "motorola,mc146818"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_end_node(fdt));
}
#else
#define generate_rtc_fdt_node NULL
#endif

struct device_header rtc_dev_hdr = {
	.bus_type = DEVICE_BUS_IOPORT,
	.data = generate_rtc_fdt_node,
};

int rtc__init(struct kvm *kvm)
{
	int r;

	r = device__register(&rtc_dev_hdr);
	if (r < 0)
		return r;

	/* PORT 0070-007F - CMOS RAM/RTC (REAL TIME CLOCK) */
	r = ioport__register(kvm, 0x0070, &cmos_ram_index_ioport_ops, 1, NULL);
	if (r < 0)
		goto out_device;

	r = ioport__register(kvm, 0x0071, &cmos_ram_data_ioport_ops, 1, NULL);
	if (r < 0)
		goto out_ioport;

	/* Set the VRT bit in Register D to indicate valid RAM and time */
	rtc.cmos_data[RTC_REG_D] = RTC_REG_D_VRT;

	return r;

out_ioport:
	ioport__unregister(kvm, 0x0070);
out_device:
	device__unregister(&rtc_dev_hdr);

	return r;
}
dev_init(rtc__init);

int rtc__exit(struct kvm *kvm)
{
	/* PORT 0070-007F - CMOS RAM/RTC (REAL TIME CLOCK) */
	ioport__unregister(kvm, 0x0070);
	ioport__unregister(kvm, 0x0071);

	return 0;
}
dev_exit(rtc__exit);
