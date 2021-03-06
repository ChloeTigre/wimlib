#ifndef _WIMLIB_ENDIANNESS_H
#define _WIMLIB_ENDIANNESS_H

#include "wimlib/types.h"

/* Watch out for conflict with ntfs-3g/endian.h ... */

#ifndef _NTFS_ENDIANS_H

static inline u16
bswap16(u16 n)
{
	return (n << 8) | (n >> 8);
}

static inline u32
bswap32(u32 n)
{
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)
	return __builtin_bswap32(n);
#else
	return (n << 24) | ((n & 0xff00) << 8) | ((n & 0xff0000) >> 8) |
							(n >> 24);
#endif
}

static inline u64
bswap64(u64 n)
{
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)
	return __builtin_bswap64(n);
#else
	return (n << 56) | ((n & 0xff00) << 40) | ((n & 0xff0000) << 24) |
			((n & 0xff000000) << 8) | ((n & 0xff00000000) >> 8) |
			((n & 0xff0000000000) >> 24) |
			((n & 0xff000000000000) >> 40) | (n >> 56);
#endif
}

#  ifdef WORDS_BIGENDIAN
#    define le16_to_cpu(n) bswap16(n)
#    define le32_to_cpu(n) bswap32(n)
#    define le64_to_cpu(n) bswap64(n)
#    define cpu_to_le16(n) bswap16(n)
#    define cpu_to_le32(n) bswap32(n)
#    define cpu_to_le64(n) bswap64(n)

#    define cpu_to_be16(n) (n)
#    define cpu_to_be32(n) (n)
#    define cpu_to_be64(n) (n)
#    define be16_to_cpu(n) (n)
#    define be32_to_cpu(n) (n)
#    define be64_to_cpu(n) (n)
#  else
#    define cpu_to_le16(n) (n)
#    define cpu_to_le32(n) (n)
#    define cpu_to_le64(n) (n)
#    define le16_to_cpu(n) (n)
#    define le32_to_cpu(n) (n)
#    define le64_to_cpu(n) (n)

#    define be16_to_cpu(n) bswap16(n)
#    define be32_to_cpu(n) bswap32(n)
#    define be64_to_cpu(n) bswap64(n)
#    define cpu_to_be16(n) bswap16(n)
#    define cpu_to_be32(n) bswap32(n)
#    define cpu_to_be64(n) bswap64(n)
#  endif
#endif /* _NTFS_ENDIANS_H */

#endif /* _WIMLIB_ENDIANNESS_H */
