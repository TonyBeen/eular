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
    mBuffer(nullptr),
    mDataSize(0),
    mCapacity(0)
{
    mCapacity = allocBuffer(dataLength);
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
        mBuffer = other.mBuffer;
        mCapacity = other.mCapacity;
        mDataSize = other.mDataSize;
        SharedBuffer::bufferFromData(mBuffer)->acquire();
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
    if (index >= mCapacity) {
        throw Exception("Index out of range");
    }

    return mBuffer[index];
}

size_t ByteBuffer::set(const uint8_t *data, size_t dataSize, size_t offset)
{
    if (data == nullptr || dataSize == 0) {
        return 0;
    }

    size_t real_offset = mDataSize >= offset ? offset : 0;
    size_t newSize = 0;

#if COMPILER_TYPE == COMPILER_GNUC || COMPILER_TYPE == COMPILER_CLANG
    if (__builtin_add_overflow(dataSize, real_offset, &newSize)) {
        return 0;
    }
#else
    newSize = real_offset + dataSize;
#endif

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) { // capacity exceeded
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(calculate(newSize));
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }
    if (buf == nullptr) {
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = buf->size();
    mDataSize = newSize;
    memmove(mBuffer + real_offset, data, dataSize);

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
    if (data == nullptr || offset > mDataSize) { // offset范围必须在0-mDataSize，等于mDataSize相当于尾插，offset=0相当于头插
        return 0;
    }

    size_t newSize = 0;
    size_t copySize = mDataSize - offset;
    size_t oldDataSize = mDataSize;
#if COMPILER_TYPE == COMPILER_GNUC || COMPILER_TYPE == COMPILER_CLANG
    if (__builtin_add_overflow(dataSize, mDataSize, &newSize)) {
        return 0;
    }
#else
    newSize = dataSize + mDataSize;
#endif

    SharedBuffer *buf = nullptr;
    if (mCapacity < newSize) {
        buf = SharedBuffer::bufferFromData(mBuffer)->editResize(calculate(newSize));
    } else {
        buf = SharedBuffer::bufferFromData(mBuffer)->edit();
    }

    if (buf == nullptr) {
        return 0;
    }

    mBuffer = static_cast<uint8_t *>(buf->data());
    mCapacity = buf->size();
    mDataSize = newSize;

    if (copySize == 0) {
        memmove(mBuffer + oldDataSize, data, dataSize);
    } else {
        uint8_t *temp = (uint8_t *)malloc(copySize);
        if (temp == nullptr) {
            return 0;
        }
        memmove(temp, mBuffer + offset, copySize);
        memmove(mBuffer + offset, data, dataSize);
        memmove(mBuffer + offset + dataSize, temp, copySize);
        free(temp);
    }

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
    int cycle = mDataSize / 4;
    for (int i = 0; i < cycle; ++i) {
        snprintf(buf, sizeof(buf), "0x%02x 0x%02x 0x%02x 0x%02x ",
            mBuffer[i * 4], mBuffer[i * 4 + 1], mBuffer[i * 4 + 2], mBuffer[i * 4 + 3]);
        ret.append(buf);
    }

    int remainder = mDataSize % 4;
    for (int i = mDataSize - remainder; i < (int)mDataSize; ++i) {
        snprintf(buf, sizeof(buf), "0x%02x ", mBuffer[i]);
        ret.append(buf);
    }

    return ret;
}

size_t ByteBuffer::Hash(const ByteBuffer &buf)
{
#if defined(OS_WINDOWS)
    return std::_Hash_array_representation(buf.const_data(), buf.size());
#else
    return std::_Hash_impl::hash(buf.const_data(), buf.size());
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
        dataSize *= 1.5;
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
        mBuffer = static_cast<uint8_t *>(SharedBuffer::alloc(size)->data());
        if (mBuffer) {
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
