/*************************************************************************
    > File Name: endian.hpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年06月15日 星期六 10时45分10秒
 ************************************************************************/

#ifndef __EULAR_UTILS_ENDIAN_HPP__
#define __EULAR_UTILS_ENDIAN_HPP__

#include <cstdint>
#include <cstdlib>

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#define UINT_IDENTIFY(X) (X)
#elif defined(OS_BSD)
#include <sys/endian.h>
#elif defined(OS_APPLE)
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#elif defined(OS_SOLARIS)
#include <sys/byteorder.h>
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

#if defined(OS_SOLARIS)
    #if defined(_LITTLE_ENDIAN)
        #define BYTE_ORDER LITTLE_ENDIAN
    #elif defined(_BIG_ENDIAN)
        #define BYTE_ORDER BIG_ENDIAN
    #endif
#endif

#ifndef BYTE_ORDER
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define BYTE_ORDER LITTLE_ENDIAN
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define BYTE_ORDER BIG_ENDIAN
    #endif
#elif defined(_M_IX86) || defined(_M_AMD64) || defined(_M_X64) ||     \
    defined(_M_ARM) || defined(_M_ARM64) ||                           \
    defined(__x86_64__) || defined(__x86_64) ||                       \
    defined(__amd64__) || defined(__amd64) ||                         \
    defined(__i386__) || defined(__i386) ||                           \
    defined(__i486__) || defined(__i586__) || defined(__i686__) ||    \
    defined(_X86_) ||                                                 \
    defined(__aarch64__) || defined(__AARCH64EL__) ||                 \
    defined(__arm__) || defined(__ARMEL__) ||                         \
    defined(__riscv) ||                                               \
    defined(__LITTLE_ENDIAN__)
    #define BYTE_ORDER LITTLE_ENDIAN
#elif defined(__ARMEB__) || defined(__AARCH64EB__) ||                 \
      defined(__MIPSEB__) || defined(__ppc__) ||                      \
      defined(__powerpc__) || defined(__BIG_ENDIAN__)
    #define BYTE_ORDER BIG_ENDIAN
#else
    #error "Unsupported byte order:  unable to detect endianness"
#endif
#endif

namespace runtime {
static inline bool IsLittleEndian();
} // namespace runtime

static inline uint64_t byteswap_64(uint64_t host_int)
{
#if EULAR_HAVE_BUILTIN(__builtin_bswap64) || (COMPILER_TYPE == COMPILER_GNUC)
    return __builtin_bswap64(host_int);
#elif (COMPILER_TYPE == COMPILER_MSVC)
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
#if EULAR_HAVE_BUILTIN(__builtin_bswap64) || (COMPILER_TYPE == COMPILER_GNUC)
  return __builtin_bswap32(host_int);
#elif (COMPILER_TYPE == COMPILER_MSVC)
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
#if EULAR_HAVE_BUILTIN(__builtin_bswap64) || (COMPILER_TYPE == COMPILER_GNUC)
  return __builtin_bswap16(host_int);
#elif (COMPILER_TYPE == COMPILER_MSVC)
  return _byteswap_ushort(host_int);
#else
  return (((host_int & uint16_t{0xFF}) << 8) |
          ((host_int & uint16_t{0xFF00}) >> 8));
#endif
}

#if defined(OS_WINDOWS)
    #if BYTE_ORDER == LITTLE_ENDIAN
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
#elif defined(OS_APPLE)
    #define htobe16(x) OSSwapHostToBigInt16(x)
    #define htobe32(x) OSSwapHostToBigInt32(x)
    #define htobe64(x) OSSwapHostToBigInt64(x)
    #define htole16(x) OSSwapHostToLittleInt16(x)
    #define htole32(x) OSSwapHostToLittleInt32(x)
    #define htole64(x) OSSwapHostToLittleInt64(x)

    #define be16toh(x) OSSwapBigToHostInt16(x)
    #define be32toh(x) OSSwapBigToHostInt32(x)
    #define be64toh(x) OSSwapBigToHostInt64(x)
    #define le16toh(x) OSSwapLittleToHostInt16(x)
    #define le32toh(x) OSSwapLittleToHostInt32(x)
    #define le64toh(x) OSSwapLittleToHostInt64(x)
#elif defined(OS_SOLARIS)
    #define htobe16(x) BE_16(x)
    #define htobe32(x) BE_32(x)
    #define htobe64(x) BE_64(x)
    #define htole16(x) LE_16(x)
    #define htole32(x) LE_32(x)
    #define htole64(x) LE_64(x)

    #define be16toh(x) BE_16(x)
    #define be32toh(x) BE_32(x)
    #define be64toh(x) BE_64(x)
    #define le16toh(x) LE_16(x)
    #define le32toh(x) LE_32(x)
    #define le64toh(x) LE_64(x)
#endif

#endif // __EULAR_UTILS_ENDIAN_HPP__
