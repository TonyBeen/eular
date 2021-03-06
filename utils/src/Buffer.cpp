/*************************************************************************
    > File Name: Buffer.cpp
    > Author: hsz
    > Mail:
    > Created Time: Mon Jul  5 13:09:00 2021
 ************************************************************************/

// #define _DEBUG
#include "Buffer.h"
#include "debug.h"
#include <assert.h>

#define DEFAULT_BUFFER_SIZE (256)

namespace eular {
ByteBuffer::ByteBuffer() : ByteBuffer(DEFAULT_BUFFER_SIZE)
{
}

ByteBuffer::ByteBuffer(size_t size) :
    mBuffer(nullptr),
    mCapacity(0),
    mDataSize(0)
{
    mCapacity = getBuffer(size);
    LOG("%s(size_t size) mCapacity = %zu\n", __func__, mCapacity);
}

ByteBuffer::ByteBuffer(const char *data, size_t dataLength) :
    mBuffer(nullptr),
    mCapacity(0),
    mDataSize(0)
{
    mCapacity = getBuffer(dataLength * 1.5);
    set((const uint8_t *)data, dataLength);
    LOG("%s(const uint8_t *data, size_t dataLength) mDataSize = %zu, mCapacity = %zu\n",
        __func__, mDataSize, mCapacity);
}

ByteBuffer::ByteBuffer(const uint8_t *data, size_t dataLength) :
    mBuffer(nullptr),
    mCapacity(0),
    mDataSize(0)
{
    mCapacity = getBuffer(dataLength * 1.5);
    set((const uint8_t *)data, dataLength);
}

ByteBuffer::ByteBuffer(const ByteBuffer& other) :
    mBuffer(nullptr),
    mCapacity(0),
    mDataSize(0)
{
    if (&other == this) {
        return;
    }

    mCapacity = getBuffer(other.size() * 1.5);
    set(other.mBuffer, other.mDataSize);
}

ByteBuffer::ByteBuffer(ByteBuffer&& other) :
    mBuffer(nullptr),
    mCapacity(0),
    mDataSize(0)
{
    if (&other == this) {
        return;
    }

    uint8_t *tmp = this->mBuffer;
    this->mBuffer = other.mBuffer;
    other.mBuffer = tmp;
    this->mDataSize = other.mDataSize;
    this->mCapacity = other.mCapacity;
}

ByteBuffer::~ByteBuffer()
{
    freeBuffer();
}

ByteBuffer& ByteBuffer::operator=(const ByteBuffer& other)
{
    if (&other == this) {
        return *this;
    }

    LOG("%s(const ByteBuffer& other)\n", __func__);
    set(other.mBuffer, other.mDataSize);
    return *this;
}

ByteBuffer& ByteBuffer::operator=(ByteBuffer& other)
{
    if (&other == this) {
        return *this;
    }

    LOG("%s(ByteBuffer& other)\n", __func__);
    set(other.mBuffer, other.mDataSize);
    return *this;
}

ByteBuffer& ByteBuffer::operator=(ByteBuffer&& other)
{
    if (&other == this) {
        return *this;
    }

    LOG("%s(ByteBuffer&& other) ???????????? mBuffer = %p, this: %s, other: %s\n",
        __func__, mBuffer, mBuffer, other.mBuffer);
    uint8_t *tmp = this->mBuffer;
    this->mBuffer = other.mBuffer;
    other.mBuffer = tmp;
    this->mDataSize = other.mDataSize;
    this->mCapacity = other.mCapacity;
    return *this;
}

uint8_t& ByteBuffer::operator[](size_t index)
{
    assert(index < mCapacity);
    return mBuffer[index];
}

size_t ByteBuffer::set(const uint8_t *data, size_t dataSize, size_t offset)
{
    if (data == nullptr || dataSize == 0) {
        return 0;
    }

    size_t real_offset = mDataSize >= offset ? offset : 0;
    uint8_t *temp = nullptr;

    if (mCapacity < (dataSize + real_offset)) {
        if (real_offset > 0 && mBuffer) {
            temp = (uint8_t *)malloc(real_offset);
            if (temp == nullptr) {
                return 0;
            }
            memcpy(temp, mBuffer, real_offset);
        }

        freeBuffer();
        mCapacity = getBuffer(calculate(dataSize + real_offset));
    }
    LOG("%s() data(%p), dataSize(%zu), real_offset(%zu), mBuffer = %p, mCapacity = %zu mDataSize = %zu\n",
            __func__, data, dataSize, real_offset, mBuffer, mCapacity, mDataSize);

    if (mCapacity > 0) {
        if (temp) {
            memcpy(mBuffer, temp, real_offset);
            free(temp);
            temp = nullptr;
        }
        memset(mBuffer + real_offset, 0, mCapacity - real_offset); // ??????real_offset???????????????
        memcpy(mBuffer + real_offset, data, dataSize);
        mDataSize = real_offset + dataSize;
        return dataSize;
    }
    if (temp) {
        free(temp);
        temp = nullptr;
    }
    return 0;
}

void ByteBuffer::append(const uint8_t *data, size_t dataSize)
{
    set(data, dataSize, size());
}

size_t ByteBuffer::insert(const uint8_t *data, size_t dataSize, size_t offset)
{
    if (data == nullptr || offset > mDataSize) { // offset???????????????0-mDataSize?????????mDataSize??????????????????offset=0???????????????
        return 0;
    }
    uint8_t *temp = nullptr;
    if (mCapacity < (dataSize + mDataSize)) {
        temp = (uint8_t *)malloc(mDataSize);
        if (temp == nullptr) {
            return 0;
        }
        memcpy(temp, mBuffer, mDataSize);

        freeBuffer();
        mCapacity = getBuffer(calculate(dataSize + offset));
    }

    if (mCapacity > 0) {
        if (temp) {
            memcpy(mBuffer, temp, offset);
            memcpy(mBuffer + offset + dataSize, temp + offset, mDataSize - offset);
            free(temp);
        } else {
            memmove(mBuffer + offset + dataSize, mBuffer + offset, mDataSize - offset);
        }
        memcpy(mBuffer + offset, data, dataSize);
        mDataSize += dataSize;
        return dataSize;
    }
    // ??????getBuffer?????????
    if (temp) {
        free(temp);
    }

    return 0;
}

void ByteBuffer::resize(size_t newSize)
{
    if (newSize == 0) {
        return;
    }

    ByteBuffer tmp(mBuffer, mDataSize);
    freeBuffer();
    mCapacity = getBuffer(newSize);
    if (mCapacity > 0) {
        size_t size = tmp.mDataSize >= mCapacity ? mCapacity : tmp.mDataSize;
        memcpy(mBuffer, tmp.mBuffer, size);
        mDataSize = size;
    }
}

void ByteBuffer::clear() 
{
    if (mBuffer) {
        memset(mBuffer, 0, mCapacity);
        mDataSize = 0;
    }
}

std::string ByteBuffer::dump() const
{
    std::string ret;
    char buf[16] = {0};
    for (int i = 0; i < mDataSize; ++i) {
        snprintf(buf, sizeof(buf), "0x%02x ", mBuffer[i]);
        ret.append(buf);
    }

    return ret;
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

size_t ByteBuffer::getBuffer(size_t size = DEFAULT_BUFFER_SIZE)
{
    if (size == 0) {
        size = DEFAULT_BUFFER_SIZE;
    }

    if (mBuffer == nullptr) {
        mBuffer = (uint8_t *)malloc(size);
        LOG("new buffer %p\n", mBuffer);
        if (mBuffer) {
            memset(mBuffer, 0, size);
            return size;
        }
    }
    return 0;
}

void ByteBuffer::freeBuffer()
{
    LOG("%s() %p\n", __func__, mBuffer);
    if (mBuffer) {
        free(mBuffer);
    }
    mBuffer = nullptr;
    mDataSize = 0;
    mCapacity = 0;
}

} // namespace eular
