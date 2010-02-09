#ifndef _CONFIG_H
#define _CONFIG_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#if 0
/* Big-endian arch */
#define le64_to_cpu(a) __swap64(a)
#define le32_to_cpu(a) __swap32(a)
#define le16_to_cpu(a) __swap16(a)
#define be64_to_cpu(a) (a)
#define be32_to_cpu(a) (a)
#define be16_to_cpu(a) (a)
#define cpu_to_le64(a) __swap64(a)
#define cpu_to_le32(a) __swap32(a)
#define cpu_to_le16(a) __swap16(a)
#define cpu_to_be64(a) (a)
#define cpu_to_be32(a) (a)
#define cpu_to_be16(a) (a)
#else
/* Little-endian arch */
#define be64_to_cpu(a) __swap64(a)
#define be32_to_cpu(a) __swap32(a)
#define be16_to_cpu(a) __swap16(a)
#define le64_to_cpu(a) (a)
#define le32_to_cpu(a) (a)
#define le16_to_cpu(a) (a)
#define cpu_to_be64(a) __swap64(a)
#define cpu_to_be32(a) __swap32(a)
#define cpu_to_be16(a) __swap16(a)
#define cpu_to_le64(a) (a)
#define cpu_to_le32(a) (a)
#define cpu_to_le16(a) (a)
#endif

static inline u16 __swap16(u16 a)
{
	return (((a & 0xff00) >> 8) | 
	        ((a & 0x00ff) << 8));
}

static inline u32 __swap32(u32 a)
{
	return (((a & 0xff000000U) >> 24) | 
	        ((a & 0x00ff0000U) >> 8) | 
	        ((a & 0x0000ff00U) << 8) | 
	        ((a & 0x000000ffU) << 24));
}

static inline u64 __swap64(u64 a)
{
	return (((a & 0xff00000000000000ULL) >> 56) |
		((a & 0x00ff000000000000ULL) >> 40) |
		((a & 0x0000ff0000000000ULL) >> 24) |
		((a & 0x000000ff00000000ULL) >> 8)  |
		((a & 0x00000000ff000000ULL) << 8)  |
		((a & 0x0000000000ff0000ULL) << 24) |
		((a & 0x000000000000ff00ULL) << 40) |
		((a & 0x00000000000000ffULL) << 56));
}


#endif /* _CONFIG_H */
