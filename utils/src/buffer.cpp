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
#include <vector>

#define DEFAULT_BUFFER_SIZE (256)

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

bool RangeOverlaps(const uint8_t *begin, size_t size, const uint8_t *data, size_t dataSize)
{
    if (begin == nullptr || data == nullptr || size == 0 || dataSize == 0) {
        return false;
    }

    const uintptr_t beginAddr = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t dataAddr = reinterpret_cast<uintptr_t>(data);
    size_t endAddr = 0;
    size_t dataEndAddr = 0;
    if (AddOverflow(beginAddr, size, &endAddr) || AddOverflow(dataAddr, dataSize, &dataEndAddr)) {
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

ByteBuffer::ByteBuffer(size_t size) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    if (size > 0) {
        SharedBuffer *newBuf = SharedBuffer::bufferFromData(mBuffer)->reset(size);
        if (newBuf != nullptr) {
            mBuffer = static_cast<uint8_t *>(newBuf->data());
            mCapacity = newBuf->size();
        }
    }
}

ByteBuffer::ByteBuffer(const char *data, size_t dataLength) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    if (data == nullptr || dataLength == 0) {
        return;
    }

    if (dataLength == SIZE_MAX) {
        dataLength = strlen(data);
    }
    set((const uint8_t *)data, dataLength);
}

ByteBuffer::ByteBuffer(const uint8_t *data, size_t dataLength) :
    mBuffer(GetEmptyBuffer()),
    mDataSize(0),
    mCapacity(0)
{
    if (data == nullptr || dataLength == 0) {
        return;
    }

    set(data, dataLength);
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

uint8_t& ByteBuffer::operator[](size_t index)
{
    if (index >= mDataSize) {
        throw Exception("Index out of range");
    }

    return mBuffer[index];
}

const uint8_t& ByteBuffer::operator[](size_t index) const
{
    if (index >= mDataSize) {
        throw Exception("Index out of range");
    }

    return mBuffer[index];
}

size_t ByteBuffer::set(const uint8_t *data, size_t dataSize, size_t offset)
{
    if (data == nullptr || dataSize == 0 || offset > mDataSize) {
        return 0;
    }

    size_t newSize = 0;
    if (AddOverflow(dataSize, offset, &newSize)) {
        return 0;
    }

    std::vector<uint8_t> temp;
    if (RangeOverlaps(mBuffer, mDataSize, data, dataSize)) {
        try {
            temp.assign(data, data + dataSize);
        } catch (...) {
            return 0;
        }
        data = temp.data();
    }

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) { // capacity exceeded
        const size_t newCapacity = calculate(newSize);
        if (newCapacity < newSize) {
            return 0;
        }
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(newCapacity);
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }
    if (buf == nullptr) {
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = buf->size();
    mDataSize = newSize;
    memmove(mBuffer + offset, data, dataSize);

    return dataSize;
}

void ByteBuffer::append(const char *data, size_t dataSize)
{
    if (data == nullptr || dataSize == 0) {
        return;
    }

    if (dataSize == SIZE_MAX) {
        dataSize = strlen(data);
    }

    set((const uint8_t *)data, dataSize, size());
}

void ByteBuffer::append(const uint8_t *data, size_t dataSize)
{
    set(data, dataSize, size());
}

void ByteBuffer::append(const ByteBuffer &other)
{
    set(other.const_data(), other.size(), size());
}

size_t ByteBuffer::insert(const uint8_t *data, size_t dataSize, size_t offset)
{
    if (data == nullptr || dataSize == 0 || offset > mDataSize) { // offset范围必须在0-mDataSize，等于mDataSize相当于尾插，offset=0相当于头插
        return 0;
    }

    size_t newSize = 0;
    size_t copySize = mDataSize - offset;
    if (AddOverflow(dataSize, mDataSize, &newSize)) {
        return 0;
    }

    std::vector<uint8_t> temp;
    if (RangeOverlaps(mBuffer, mDataSize, data, dataSize)) {
        try {
            temp.assign(data, data + dataSize);
        } catch (...) {
            return 0;
        }
        data = temp.data();
    }

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) {
        const size_t newCapacity = calculate(newSize);
        if (newCapacity < newSize) {
            return 0;
        }
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(newCapacity);
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }

    if (buf == nullptr) {
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = buf->size();
    if (copySize > 0) {
        memmove(mBuffer + offset + dataSize, mBuffer + offset, copySize);
    }
    memmove(mBuffer + offset, data, dataSize);
    mDataSize = newSize;

    return dataSize;
}

void ByteBuffer::reserve(size_t newSize)
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

    SharedBuffer *buf = SharedBuffer::bufferFromData(mBuffer)->editResize(newSize);
    if (buf) {
        mBuffer = static_cast<uint8_t *>(buf->data());
        mCapacity = buf->size();
    }
}

void ByteBuffer::clear() 
{
    detach();

#ifdef _DEBUG
    if (mBuffer) {
        memset(mBuffer, 0, mCapacity);
    }
#endif
    mDataSize = 0;
}

void ByteBuffer::resize(size_t sz)
{
    reserve(sz);
    if (mBuffer) {
        mDataSize = sz;
    }
}

std::string ByteBuffer::dump() const
{
    std::string ret;
    if (mBuffer == nullptr || mDataSize == 0) {
        return ret;
    }

    char buf[64] = {0};
    if (mDataSize > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        return ret;
    }

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
    return std::_Hash_array_representation(buf.const_data(), buf.size());
#elif defined(OS_LINUX)
    return std::_Hash_impl::hash(buf.const_data(), buf.size());
#else
    // MacOS and others
    // FNV-1a hash algorithm
    const uint8_t *data = buf.const_data();
    size_t length = buf.size();

#if UINTPTR_MAX == UINT64_MAX
    size_t hash = 14695981039346656037ULL;
    const size_t prime = 1099511628211ULL;
#else
    size_t hash = 2166136261U;
    const size_t prime = 16777619U;
#endif

    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<size_t>(data[i]);
        hash *= prime;
    }

    return hash;
#endif
}

bool ByteBuffer::operator==(const ByteBuffer &other) const
{
    // 共享状态下一定是同一份数据
    if (mBuffer == other.mBuffer) {
        return true;
    }

    if (mDataSize != other.mDataSize) {
        return false;
    }

    return 0 == memcmp(mBuffer, other.mBuffer, mDataSize);
}

size_t ByteBuffer::calculate(size_t dataSize)
{
    if (dataSize >= DEFAULT_BUFFER_SIZE) {
        const size_t extra = dataSize / 2;
        if (dataSize > std::numeric_limits<size_t>::max() - extra) {
            return 0;
        }
        dataSize += extra;
    } else {
        dataSize = DEFAULT_BUFFER_SIZE;
    }
    return dataSize;
}

size_t ByteBuffer::allocBuffer(size_t size)
{
    if (size == 0) {
        size = DEFAULT_BUFFER_SIZE;
    }

    if (mBuffer == nullptr) {
        SharedBuffer *sb = SharedBuffer::alloc(size);
        if (sb != nullptr) {
            mBuffer = static_cast<uint8_t *>(sb->data());
#ifdef _DEBUG
            memset(mBuffer, 0, size);
#endif
            return size;
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

void ByteBuffer::detach()
{
    SharedBuffer *psb = SharedBuffer::bufferFromData(mBuffer);
    if (psb == nullptr) {
        mCapacity = allocBuffer(DEFAULT_BUFFER_SIZE);
        mDataSize = 0;
        return;
    }

    if (psb->onlyOwner() == false) {
        SharedBuffer *newSb = psb->edit();
        if (newSb) {
            mBuffer = static_cast<uint8_t *>(newSb->data());
        }
    }
}

} // namespace eular
