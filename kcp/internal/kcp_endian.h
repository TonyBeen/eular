#ifndef __KCP_ENDIAN_H__
#define __KCP_ENDIAN_H__


#include <stdint.h>
#include <stdlib.h>

#include <kcp_def.h>

#if defined(OS_WIN)
#define UINT_IDENTIFY(X) (X)
#elif defined(OS_BSD)
#include <sys/endian.h>
#elif defined(OS_DARWIN)
#include <machine/endian.h>
#else
#include <endian.h>
#endif

#ifndef BIG_ENDIAN
#define BIG_ENDIAN      4321
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN   1234
#endif
#ifndef NET_ENDIAN
#define NET_ENDIAN      BIG_ENDIAN
#endif

#ifndef BYTE_ORDER
#if defined(_X86_) || defined(__x86_64__) || defined(__i386__) || \
    defined(__i486__) || defined(__i586__) || defined(__i686__) || \
    defined(__ARMEL__) || defined(__AARCH64EL__) || \
    defined(_M_ARM) || defined(_M_ARM64) || \ // microsoft ARM64
    defined(_M_IX86) || defined(_M_AMD64) || defined(_M_X64)
    #define BYTE_ORDER      LITTLE_ENDIAN
#elif defined(__ARMEB__) || defined(__AARCH64EB__)
    #define BYTE_ORDER      BIG_ENDIA
#else
    #error "Unsupported byte order"
#endif
#endif

EXTERN_C_BEGIN

static inline uint64_t byteswap_64(uint64_t host_int)
{
#if defined(__GNUC__)
    return __builtin_bswap64(host_int);
#elif defined( _MSC_VER )
    return _byteswap_uint64(host_int);
#else
    return (((host_int & uint64_t{0xFF}) << 56) |
            ((host_int & uint64_t{0xFF00}) << 40) |
            ((host_int & uint64_t{0xFF0000}) << 24) |
            ((host_int & uint64_t{0xFF000000}) << 8) |
            ((host_int & uint64_t{0xFF00000000}) >> 8) |
            ((host_int & uint64_t{0xFF0000000000}) >> 24) |
            ((host_int & uint64_t{0xFF000000000000}) >> 40) |
            ((host_int & uint64_t{0xFF00000000000000}) >> 56));
#endif
}

static inline uint32_t byteswap_32(uint32_t host_int)
{
#if defined(__GNUC__)
    return __builtin_bswap32(host_int);
#elif defined( _MSC_VER )
    return _byteswap_ulong(host_int);
#else
    return (((host_int & uint32_t{0xFF}) << 24) |
            ((host_int & uint32_t{0xFF00}) << 8) |
            ((host_int & uint32_t{0xFF0000}) >> 8) |
            ((host_int & uint32_t{0xFF000000}) >> 24));
#endif
}

static inline uint16_t byteswap_16(uint16_t host_int)
{
#if defined(__GNUC__)
    return __builtin_bswap16(host_int);
#elif defined( _MSC_VER )
    return _byteswap_ushort(host_int);
#else
    return (((host_int & uint16_t{0xFF}) << 8) |
            ((host_int & uint16_t{0xFF00}) >> 8));
#endif
}

EXTERN_C_END

#if defined(OS_WIN)
# if BYTE_ORDER == LITTLE_ENDIAN
#define htobe16(x) byteswap_16 (x)
#define htole16(x) UINT_IDENTIFY (x)
#define be16toh(x) byteswap_16 (x)
#define le16toh(x) UINT_IDENTIFY (x)

#define htobe32(x) byteswap_32 (x)
#define htole32(x) UINT_IDENTIFY (x)
#define be32toh(x) byteswap_32 (x)
#define le32toh(x) UINT_IDENTIFY (x)

#define htobe64(x) byteswap_64 (x)
#define htole64(x) UINT_IDENTIFY (x)
#define be64toh(x) byteswap_64 (x)
#define le64toh(x) UINT_IDENTIFY (x)

#else

#define htobe16(x) UINT_IDENTIFY (x)
#define htole16(x) byteswap_16 (x)
#define be16toh(x) UINT_IDENTIFY (x)
#define le16toh(x) byteswap_16 (x)

#define htobe32(x) UINT_IDENTIFY (x)
#define htole32(x) byteswap_32 (x)
#define be32toh(x) UINT_IDENTIFY (x)
#define le32toh(x) byteswap_32 (x)

#define htobe64(x) UINT_IDENTIFY (x)
#define htole64(x) byteswap_64 (x)
#define be64toh(x) UINT_IDENTIFY (x)
#define le64toh(x) byteswap_64 (x)
#endif
#endif

#endif // __KCP_ENDIAN_H__
