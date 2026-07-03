/*************************************************************************
    > File Name: Buffer.cpp
    > Author: hsz
    > Mail:
    > Created Time: Mon Jul  5 13:09:00 2021
 ************************************************************************/

#include "utils/buffer.h"
#include "utils/shared_buffer.h"
#include "utils/exception.h"
#include "utils/sysdef.h"

#include <assert.h>
#include <limits>

#define DEFAULT_BUFFER_SIZE (256U)

// 此函数会导致valgrind报错: in use at exit: 25 bytes in 1 blocks
static inline uint8_t *GetEmptyBuffer()
{
    static eular::SharedBuffer* gEmptyBuffer = [] {
        eular::SharedBuffer* buf = eular::SharedBuffer::alloc(1);
        char* str = static_cast<char*>(buf->data());
        *str = 0;
        return buf;
    }();

    gEmptyBuffer->acquire();
    return static_cast<uint8_t *>(gEmptyBuffer->data());
}

namespace eular {
namespace {

bool AddOverflow(size_t lhs, size_t rhs, size_t *result)
{
#if COMPILER_TYPE == COMPILER_GNUC || COMPILER_TYPE == COMPILER_CLANG
    return __builtin_add_overflow(lhs, rhs, result);
#else
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        return true;
    }
    *result = lhs + rhs;
    return false;
#endif
}

bool AddBufferSize(uint32_t lhs, uint32_t rhs, uint32_t *result)
{
    const uint64_t sum = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs);
    if (sum > UINT32_MAX) {
        return false;
    }

    *result = static_cast<uint32_t>(sum);
    return true;
}

bool ToAllocSize(uint32_t size, size_t *out)
{
    if (static_cast<uint64_t>(size) >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max() - sizeof(SharedBuffer))) {
        return false;
    }

    *out = static_cast<size_t>(size);
    return true;
}

bool RangeOverlaps(const uint8_t *begin, uint32_t size, const uint8_t *data, uint32_t dataSize)
{
    if (begin == nullptr || data == nullptr || size == 0 || dataSize == 0) {
        return false;
    }

    const uintptr_t beginAddr = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t dataAddr = reinterpret_cast<uintptr_t>(data);
    size_t endAddr = 0;
    size_t dataEndAddr = 0;
    if (AddOverflow(beginAddr, static_cast<size_t>(size), &endAddr) ||
        AddOverflow(dataAddr, static_cast<size_t>(dataSize), &dataEndAddr)) {
        return false;
    }

    return dataAddr < endAddr && dataEndAddr > beginAddr;
}

} // namespace

ByteBuffer::ByteBuffer():
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
}

ByteBuffer::ByteBuffer(uint32_t size) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    size_t allocSize = 0;
    if (size > 0 && ToAllocSize(size, &allocSize)) {
        SharedBuffer *newBuf = SharedBuffer::bufferFromData(mBuffer)->reset(allocSize);
        if (newBuf != nullptr) {
            mBuffer = static_cast<uint8_t *>(newBuf->data());
            mCapacity = static_cast<uint32_t>(newBuf->size());
        }
    }
}

ByteBuffer::ByteBuffer(const char *data, uint32_t dataLength) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    if (data == nullptr || dataLength == 0) {
        return;
    }

    if (dataLength == UINT32_MAX) {
        const size_t length = strlen(data);
        if (length >= static_cast<size_t>(UINT32_MAX)) {
            return;
        }
        dataLength = static_cast<uint32_t>(length);
    }
    set((const uint8_t *)data, dataLength);
}

ByteBuffer::ByteBuffer(const uint8_t *data, uint32_t dataLength) :
    mBuffer(nullptr),
    mDataSize(0),
    mCapacity(0)
{
    if (data == nullptr || dataLength == 0) {
        mBuffer = GetEmptyBuffer();
        return;
    }

    mCapacity = allocBuffer(dataLength);
    if (mBuffer == nullptr || set(data, dataLength) != dataLength) {
        freeBuffer();
        mBuffer = GetEmptyBuffer();
    }
}

ByteBuffer::ByteBuffer(const ByteBuffer& other) :
    mBuffer(other.mBuffer),
    mDataSize(other.mDataSize),
    mCapacity(other.mCapacity)
{
    SharedBuffer::bufferFromData(mBuffer)->acquire();
}

ByteBuffer::ByteBuffer(ByteBuffer&& other) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    if (std::addressof(other) == this) {
        return;
    }

    moveAssign(other);
}

ByteBuffer::~ByteBuffer()
{
    freeBuffer();
}

ByteBuffer& ByteBuffer::operator=(const ByteBuffer& other)
{
    if (std::addressof(other) != this) {
        if (other.mBuffer) {
            SharedBuffer::bufferFromData(other.mBuffer)->acquire();
        }

        freeBuffer();
        mBuffer = other.mBuffer;
        mCapacity = other.mCapacity;
        mDataSize = other.mDataSize;
    }

    return *this;
}

ByteBuffer& ByteBuffer::operator=(ByteBuffer&& other)
{
    if (std::addressof(other) != this) {
        moveAssign(other);
    }

    return *this;
}

uint8_t* ByteBuffer::data()
{
    if (!detach()) {
        throw Exception("ByteBuffer::data() detach failed");
    }
    return mBuffer;
}

uint8_t& ByteBuffer::operator[](uint32_t index)
{
    if (index >= mDataSize) {
        throw Exception("Index out of range");
    }

    return mBuffer[index];
}

const uint8_t& ByteBuffer::operator[](uint32_t index) const
{
    if (index >= mDataSize) {
        throw Exception("Index out of range");
    }

    return mBuffer[index];
}

uint32_t ByteBuffer::set(const uint8_t *data, uint32_t dataSize, uint32_t offset)
{
    if (data == nullptr || dataSize == 0 || offset > mDataSize) {
        return 0;
    }

    uint32_t newSize = 0;
    if (!AddBufferSize(dataSize, offset, &newSize)) {
        return 0;
    }

    uint8_t *temp = nullptr;
    if (RangeOverlaps(mBuffer, mDataSize, data, dataSize)) {
        size_t tempSize = 0;
        if (!ToAllocSize(dataSize, &tempSize)) {
            return 0;
        }
        temp = static_cast<uint8_t *>(malloc(tempSize));
        if (temp == nullptr) {
            return 0;
        }
        memcpy(temp, data, tempSize);
        data = temp;
    }

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) { // capacity exceeded
        const uint32_t newCapacity = calculate(newSize);
        if (newCapacity < newSize) {
            free(temp);
            return 0;
        }
        size_t allocSize = 0;
        if (!ToAllocSize(newCapacity, &allocSize)) {
            free(temp);
            return 0;
        }
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(allocSize);
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }
    if (buf == nullptr) {
        free(temp);
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = static_cast<uint32_t>(buf->size());
    mDataSize = newSize;
    memmove(mBuffer + static_cast<size_t>(offset), data, static_cast<size_t>(dataSize));
    free(temp);

    return static_cast<uint32_t>(dataSize);
}

void ByteBuffer::append(const char *data, uint32_t dataSize)
{
    if (data == nullptr || dataSize == 0) {
        return;
    }

    if (dataSize == UINT32_MAX) {
        const size_t length = strlen(data);
        if (length >= static_cast<size_t>(UINT32_MAX)) {
            return;
        }
        dataSize = static_cast<uint32_t>(length);
    }

    set((const uint8_t *)data, dataSize, size());
}

void ByteBuffer::append(const uint8_t *data, uint32_t dataSize)
{
    set(data, dataSize, size());
}

void ByteBuffer::append(const ByteBuffer &other)
{
    set(other.const_data(), other.size(), size());
}

uint32_t ByteBuffer::insert(const uint8_t *data, uint32_t dataSize, uint32_t offset)
{
    if (data == nullptr || dataSize == 0 || offset > mDataSize) { // offset范围必须在0-mDataSize，等于mDataSize相当于尾插，offset=0相当于头插
        return 0;
    }

    uint32_t newSize = 0;
    uint32_t copySize = mDataSize - offset;
    if (!AddBufferSize(dataSize, mDataSize, &newSize)) {
        return 0;
    }

    uint8_t *temp = nullptr;
    if (RangeOverlaps(mBuffer, mDataSize, data, dataSize)) {
        size_t tempSize = 0;
        if (!ToAllocSize(dataSize, &tempSize)) {
            return 0;
        }
        temp = static_cast<uint8_t *>(malloc(tempSize));
        if (temp == nullptr) {
            return 0;
        }
        memcpy(temp, data, tempSize);
        data = temp;
    }

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) {
        const uint32_t newCapacity = calculate(newSize);
        if (newCapacity < newSize) {
            free(temp);
            return 0;
        }
        size_t allocSize = 0;
        if (!ToAllocSize(newCapacity, &allocSize)) {
            free(temp);
            return 0;
        }
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(allocSize);
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }

    if (buf == nullptr) {
        free(temp);
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = static_cast<uint32_t>(buf->size());
    if (copySize > 0) {
        memmove(mBuffer + static_cast<size_t>(offset + dataSize), mBuffer + static_cast<size_t>(offset), static_cast<size_t>(copySize));
    }
    memmove(mBuffer + static_cast<size_t>(offset), data, static_cast<size_t>(dataSize));
    mDataSize = newSize;
    free(temp);

    return static_cast<uint32_t>(dataSize);
}

void ByteBuffer::reserve(uint32_t newSize)
{
    if (newSize == 0) {
        return;
    }

    // NOTE 移动构造的mBuffer可能会是null
    if (mBuffer == nullptr) {
        mCapacity = allocBuffer(newSize);
        mDataSize = 0;
        return;
    }

    if (mCapacity >= newSize) {
        return;
    }

    size_t allocSize = 0;
    if (!ToAllocSize(newSize, &allocSize)) {
        return;
    }

    SharedBuffer *buf = SharedBuffer::bufferFromData(mBuffer)->editResize(allocSize);
    if (buf) {
        mBuffer = static_cast<uint8_t *>(buf->data());
        mCapacity = static_cast<uint32_t>(buf->size());
    }
}

void ByteBuffer::clear() 
{
    if (!detach()) {
        throw Exception("ByteBuffer::clear() detach failed");
    }

#ifdef _DEBUG
    if (mBuffer) {
        size_t cap = 0;
        if (ToAllocSize(mCapacity, &cap)) {
            memset(mBuffer, 0, cap);
        }
    }
#endif
    mDataSize = 0;
}

void ByteBuffer::resize(uint32_t sz)
{
    reserve(sz);
    if (mBuffer && mCapacity >= sz) {
        if (!detach()) {
            throw Exception("ByteBuffer::resize() detach failed");
        }
        mDataSize = static_cast<uint32_t>(sz);
    }
}

std::string ByteBuffer::dump() const
{
    std::string ret;
    if (mBuffer == nullptr || mDataSize == 0) {
        return ret;
    }

    char buf[64] = {0};
    int64_t cycle = static_cast<int64_t>(mDataSize / 4);
    for (int64_t i = 0; i < cycle; ++i) {
        snprintf(buf, sizeof(buf), "0x%02x 0x%02x 0x%02x 0x%02x ",
            mBuffer[i * 4], mBuffer[i * 4 + 1], mBuffer[i * 4 + 2], mBuffer[i * 4 + 3]);
        ret.append(buf);
    }

    int64_t remainder = static_cast<int64_t>(mDataSize % 4);
    for (int64_t i = static_cast<int64_t>(mDataSize) - remainder; i < static_cast<int64_t>(mDataSize); ++i) {
        snprintf(buf, sizeof(buf), "0x%02x ", mBuffer[i]);
        ret.append(buf);
    }

    return ret;
}

size_t ByteBuffer::Hash(const ByteBuffer &buf)
{
#if defined(OS_WINDOWS)
    size_t len = 0;
    if (!ToAllocSize(buf.size(), &len)) {
        return 0;
    }
    return std::_Hash_array_representation(buf.const_data(), len);
#elif defined(OS_LINUX)
    size_t len = 0;
    if (!ToAllocSize(buf.size(), &len)) {
        return 0;
    }
    return std::_Hash_impl::hash(buf.const_data(), len);
#else
    // MacOS and others
    // FNV-1a hash algorithm
    const uint8_t *data = buf.const_data();
    uint64_t length = buf.size();

#if UINTPTR_MAX == UINT64_MAX
    size_t hash = 14695981039346656037ULL;
    const size_t prime = 1099511628211ULL;
#else
    size_t hash = 2166136261U;
    const size_t prime = 16777619U;
#endif

    for (uint64_t i = 0; i < length; ++i) {
        hash ^= static_cast<size_t>(data[i]);
        hash *= prime;
    }

    return hash;
#endif
}

bool ByteBuffer::operator==(const ByteBuffer &other) const
{
    if (mDataSize != other.mDataSize) {
        return false;
    }

    if (mDataSize == 0 || mBuffer == other.mBuffer) {
        return true;
    }

    return 0 == memcmp(mBuffer, other.mBuffer, static_cast<size_t>(mDataSize));
}

uint32_t ByteBuffer::calculate(uint32_t dataSize)
{
    if (dataSize >= DEFAULT_BUFFER_SIZE) {
        const uint32_t extra = dataSize / 2;
        dataSize = dataSize > UINT32_MAX - extra
            ? UINT32_MAX
            : dataSize + extra;
    } else {
        dataSize = DEFAULT_BUFFER_SIZE;
    }
    return dataSize;
}

uint32_t ByteBuffer::allocBuffer(uint32_t size)
{
    if (size == 0) {
        size = DEFAULT_BUFFER_SIZE;
    }

    if (mBuffer == nullptr) {
        size_t allocSize = 0;
        if (!ToAllocSize(size, &allocSize)) {
            return 0;
        }

        SharedBuffer *sb = SharedBuffer::alloc(allocSize);
        if (sb != nullptr) {
            mBuffer = static_cast<uint8_t *>(sb->data());
#ifdef _DEBUG
            memset(mBuffer, 0, allocSize);
#endif
            return static_cast<uint32_t>(sb->size());
        }
    }

    return 0;
}

void ByteBuffer::freeBuffer()
{
    if (mBuffer) {
        SharedBuffer::bufferFromData(mBuffer)->release();
    }
    mBuffer = nullptr;
    mDataSize = 0;
    mCapacity = 0;
}

void ByteBuffer::moveAssign(ByteBuffer &other)
{
    std::swap(mBuffer, other.mBuffer);
    std::swap(mDataSize, other.mDataSize);
    std::swap(mCapacity, other.mCapacity);
}

bool ByteBuffer::detach()
{
    SharedBuffer *psb = SharedBuffer::bufferFromData(mBuffer);
    if (psb == nullptr) {
        mCapacity = allocBuffer(DEFAULT_BUFFER_SIZE);
        mDataSize = 0;
        return mBuffer != nullptr;
    }

    if (psb->onlyOwner() == false) {
        SharedBuffer *newSb = psb->edit();
        if (newSb) {
            mBuffer = static_cast<uint8_t *>(newSb->data());
            mCapacity = static_cast<uint32_t>(newSb->size());
        } else {
            return false;
        }
    }

    return true;
}

} // namespace eular
