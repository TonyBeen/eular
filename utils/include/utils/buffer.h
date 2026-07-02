/*************************************************************************
    > File Name: Buffer.h
    > Author: hsz
    > Mail:
    > Created Time: Mon Jul  5 13:08:56 2021
 ************************************************************************/

#ifndef __EULAR_BUFFER_H__
#define __EULAR_BUFFER_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <utils/sysdef.h>

namespace eular {
class UTILS_API ByteBuffer final
{
public:
    ByteBuffer();
    ByteBuffer(uint64_t size);
    ByteBuffer(const char *data, uint64_t dataLength = UINT64_MAX);
    ByteBuffer(const uint8_t *data, uint64_t dataLength);
    ByteBuffer(const ByteBuffer& other);
    ByteBuffer(ByteBuffer&& other);
    ~ByteBuffer();

    ByteBuffer&     operator=(const ByteBuffer& other);
    ByteBuffer&     operator=(ByteBuffer&& other);
    uint8_t&        operator[](uint64_t index);
    const uint8_t&  operator[](uint64_t index) const;

    // 在offset之后设为data
    uint64_t        set(const uint8_t *data, uint64_t dataSize, uint64_t offset = 0);
    void            append(const char *data, uint64_t dataSize = UINT64_MAX);
    void            append(const uint8_t *data, uint64_t dataSize);
    void            append(const ByteBuffer &other);
    // 在offset之后插入数据而不覆盖之后的数据
    uint64_t        insert(const uint8_t *data, uint64_t dataSize, uint64_t offset = 0);

    uint8_t*        data();
    const uint8_t*  const_data() const noexcept { return mBuffer; }
    const uint8_t*  begin() const noexcept { return mBuffer; }
    const uint8_t*  end() const noexcept { return mBuffer ? (mBuffer + mDataSize) : nullptr; }
    void            reserve(uint64_t newSize);
    uint64_t        capacity() const noexcept { return mCapacity; }
    uint64_t        size() const noexcept { return mDataSize; }
    void            clear();
    void            resize(uint64_t sz);

    std::string     dump()  const;
    bool            operator==(const ByteBuffer &other) const;
    
    static size_t   Hash(const ByteBuffer &buf);
    static void*    GLIBC_memmem(const void *haystack, size_t hs_len, const void *needle, size_t ne_len);

private:
    uint64_t        calculate(uint64_t);
    uint64_t        allocBuffer(uint64_t size);
    void            freeBuffer();
    void            moveAssign(ByteBuffer &other);
    bool            detach();

private:
    uint8_t*    mBuffer;
    uint64_t    mDataSize;
    uint64_t    mCapacity;
};

} // namespace eular

namespace std {
    template<>
    struct hash<eular::ByteBuffer> {
        size_t operator()(const eular::ByteBuffer &obj) const {
            return eular::ByteBuffer::Hash(obj);
        }
    };
}

#endif // __EULAR_BUFFER_H__
