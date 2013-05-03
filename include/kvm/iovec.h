#ifndef KVM_UTIL_IOVEC_H_
#define KVM_UTIL_IOVEC_H_

extern int memcpy_fromiovec(unsigned char *kdata, struct iovec *iov, int len);
extern int memcpy_fromiovecend(unsigned char *kdata, const struct iovec *iov,
				size_t offset, int len);
extern int memcpy_toiovec(struct iovec *v, unsigned char *kdata, int len);
extern int memcpy_toiovecend(const struct iovec *v, unsigned char *kdata,
				size_t offset, int len);

static inline size_t iov_size(const struct iovec *iovec, size_t len)
{
	size_t size = 0, i;

	for (i = 0; i < len; i++)
		size += iovec[i].iov_len;

	return size;
}

#endif
