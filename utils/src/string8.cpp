/*************************************************************************
    > File Name: string8.cpp
    > Author: hsz
    > Mail:
    > Created Time: 2021年04月11日 星期日 12时25分02秒
 ************************************************************************/

#include "utils/string8.h"

#include <assert.h>
#include <algorithm>

#include "utils/platform.h"
#include "utils/utils.h"
#include "utils/shared_buffer.h"
#include "utils/debug.h"
#include "utils/errors.h"
#include "utils/exception.h"
#include "src/printf.h"

#define MIN_HEAP_STRING_SIZE    64
#define MAX_STRING_SIZE         (8 * 1024 * 1024) // 8Mb

namespace eular {

// ============== 构造与析构 ==============

String8::String8()
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
}

String8::String8(uint32_t size)
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
    if (size > LOCAL_STRING_SIZE - 1) {
        mHeap = allocHeap(size);
        mString = mHeap;
        mCapacity = size;
    }
}

String8::String8(const String8& other)
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    initFromChars(other.mString, other.mLength);
}

String8::String8(const char* other)
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
    if (other) {
        initFromChars(other, strlen(other));
    }
}

String8::String8(const char* other, size_t numChars)
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
    if (other && numChars > 0) {
        initFromChars(other, numChars);
    }
}

String8::String8(const std::string& other)
    : mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
    initFromChars(other.c_str(), other.length());
}

String8::String8(String8 &&other) noexcept
    :  mString(mStack)
    , mLength(0)
    , mCapacity(LOCAL_STRING_SIZE - 1)
{
    mStack[0] = '\0';
    
    if (other.isLocal()) {
        // 源使用栈存储，直接拷贝
        memcpy(mStack, other.mStack, other.mLength + 1);
        mLength = other.mLength;
        mString = mStack;
        mCapacity = LOCAL_STRING_SIZE - 1;
    } else {
        // 源使用堆存储，转移所有权
        mHeap = other.mHeap;
        mString = mHeap;
        mLength = other.mLength;
        mCapacity = other.mCapacity;
        
        // 重置源对象为栈状态
        other. mString = other.mStack;
        other.mStack[0] = '\0';
        other. mLength = 0;
        other. mCapacity = LOCAL_STRING_SIZE - 1;
    }
}

String8::~String8()
{
    release();
}

// ============== 核心辅助函数 ==============

void String8::initFromChars(const char* str, size_t len)
{
    if (str == nullptr || len == 0) {
        mString = mStack;
        mStack[0] = '\0';
        mLength = 0;
        mCapacity = LOCAL_STRING_SIZE - 1;
        return;
    }

    if (len >= MAX_STRING_SIZE) {
        throw Exception("String8: string too long");
    }

    if (len < LOCAL_STRING_SIZE) {
        // 使用栈存储
        memcpy(mStack, str, len);
        mStack[len] = '\0';
        mString = mStack;
        mLength = static_cast<uint32_t>(len);
        mCapacity = LOCAL_STRING_SIZE - 1;
    } else {
        // 使用堆存储
        mHeap = allocHeap(len);
        memcpy(mHeap, str, len);
        mHeap[len] = '\0';
        mString = mHeap;
        mLength = static_cast<uint32_t>(len);
        mCapacity = static_cast<uint32_t>(len);
    }
}

char* String8::allocHeap(size_t numChars)
{
    if (numChars >= MAX_STRING_SIZE) {
        throw Exception("String8::allocHeap() too many characters");
    }

    size_t allocSize = numChars < 64 ? 64 : numChars + 1;
    SharedBuffer *psb = SharedBuffer::alloc(allocSize);
    if (psb == nullptr) {
        throw Exception("Unable to allocate shared buffer");
    }

    char *buf = static_cast<char *>(psb->data());
    buf[0] = '\0';
    return buf;
}

void String8::release()
{
    if (!isLocal() && mHeap != nullptr) {
        SharedBuffer::bufferFromData(mHeap)->release();
    }

    // 重置为栈状态
    mString = mStack;
    mStack[0] = '\0';
    mLength = 0;
    mCapacity = LOCAL_STRING_SIZE - 1;
}

void String8::ensureUnique()
{
    if (isLocal()) {
        return; // 栈存储不需要处理
    }

    SharedBuffer *psb = SharedBuffer::bufferFromData(mHeap);
    if (psb && !psb->onlyOwner()) {
        // 需要拷贝一份
        char* newBuf = allocHeap(mLength);
        memcpy(newBuf, mHeap, mLength + 1);
        psb->release();
        mHeap = newBuf;
        mString = mHeap;
    }
}

// ============== 基本访问方法 ==============

const char* String8::c_str() const
{
    return mString;
}

char* String8::data()
{
    ensureUnique();
    return mString;
}

bool String8::empty() const
{
    return mLength == 0;
}

size_t String8::length() const
{
    return mLength;
}

void String8::clear()
{
    if (isLocal()) {
        mStack[0] = '\0';
        mLength = 0;
    } else {
        ensureUnique();
        mHeap[0] = '\0';
        mLength = 0;
    }
}

char& String8::front()
{
    if (empty()) {
        throw Exception("String8::front() on empty string");
    }
    ensureUnique();
    return mString[0];
}

const char& String8::front() const
{
    if (empty()) {
        throw Exception("String8::front() on empty string");
    }
    return mString[0];
}

char& String8::back()
{
    if (empty()) {
        throw Exception("String8::back() on empty string");
    }
    ensureUnique();
    return mString[mLength - 1];
}

const char& String8::back() const
{
    if (empty()) {
        throw Exception("String8::back() on empty string");
    }
    return mString[mLength - 1];
}

std::string String8::toStdString() const
{
    return std::string(mString, mLength);
}

// ============== 容量操作 ==============

void String8::reserve(size_t size)
{
    if (size <= mCapacity) {
        ensureUnique();
        return;
    }

    if (size >= MAX_STRING_SIZE) {
        throw Exception("String8::reserve() size too large");
    }

    // 需要扩容，必须使用堆
    char* newBuf = allocHeap(size);
    if (mLength > 0) {
        memcpy(newBuf, mString, mLength + 1);
    } else {
        newBuf[0] = '\0';
    }

    if (! isLocal()) {
        SharedBuffer::bufferFromData(mHeap)->release();
    }

    mHeap = newBuf;
    mString = mHeap;
    mCapacity = static_cast<uint32_t>(size);
}

void String8::resize(size_t size)
{
    if (size == mLength) {
        return;
    }

    if (size < mLength) {
        // 缩小
        ensureUnique();
        mString[size] = '\0';
        mLength = static_cast<uint32_t>(size);
    } else {
        // 扩大
        reserve(size);
        memset(mString + mLength, '\0', size - mLength + 1);
        mLength = static_cast<uint32_t>(size);
    }
}

// ============== 修改操作 ==============

int32_t String8::append(char ch)
{
    return append(&ch, 1);
}

int32_t String8::append(const String8& other)
{
    return append(other.mString, other.mLength);
}

int32_t String8::append(const char* other)
{
    if (other == nullptr) {
        return 0;
    }
    return append(other, strlen(other));
}

int32_t String8::append(const char* other, size_t numChars)
{
    if (other == nullptr || numChars == 0) {
        return 0;
    }

    size_t otherLen = numChars;
    size_t newLen = mLength + otherLen;
    if (newLen >= MAX_STRING_SIZE) { // 新字符串超过最大长度, 不拷贝任何字符
        return 0;
    }

    if (newLen < LOCAL_STRING_SIZE && isLocal()) {
        // 仍可使用栈存储
        memcpy(mStack + mLength, other, otherLen);
        mStack[newLen] = '\0';
        mLength = static_cast<uint32_t>(newLen);
        return static_cast<int32_t>(otherLen);
    }

    if (newLen <= mCapacity) {
        // 容量足够
        ensureUnique();
        memcpy(mString + mLength, other, otherLen);
        mString[newLen] = '\0';
        mLength = static_cast<uint32_t>(newLen);
        return static_cast<int32_t>(otherLen);
    }

    // 需要扩容
    size_t newCapacity = std::max(newLen, (size_t)mCapacity * 2);
    char* newBuf = allocHeap(newCapacity);

    if (mLength > 0) {
        memcpy(newBuf, mString, mLength);
    }
    memcpy(newBuf + mLength, other, otherLen);
    newBuf[newLen] = '\0';

    if (!isLocal()) {
        SharedBuffer::bufferFromData(mHeap)->release();
    }

    mHeap = newBuf;
    mString = mHeap;
    mLength = static_cast<uint32_t>(newLen);
    mCapacity = static_cast<uint32_t>(newCapacity);

    return static_cast<int32_t>(otherLen);
}

int32_t String8::setTo(const char* other)
{
    if (other == nullptr) {
        clear();
        return 0;
    }
    return setTo(other, strlen(other));
}

int32_t String8::setTo(const char* other, size_t numChars)
{
    if (other == nullptr || numChars == 0) {
        clear();
        return 0;
    }

    size_t actualLen = strnlen(other, numChars);
    if (actualLen >= MAX_STRING_SIZE) {
        return -1;
    }

    // 检查是否自赋值
    if (other >= mString && other < mString + mLength) {
        // 自赋值情况，需要特殊处理
        String8 temp(other, actualLen);
        *this = std::move(temp);
        return static_cast<int32_t>(actualLen);
    }

    if (actualLen < LOCAL_STRING_SIZE) {
        // 可以使用栈存储
        if (! isLocal()) {
            SharedBuffer::bufferFromData(mHeap)->release();
        }
        memcpy(mStack, other, actualLen);
        mStack[actualLen] = '\0';
        mString = mStack;
        mLength = static_cast<uint32_t>(actualLen);
        mCapacity = LOCAL_STRING_SIZE - 1;
    } else if (actualLen <= mCapacity && ! isLocal()) {
        // 可以复用现有堆内存
        ensureUnique();
        memcpy(mHeap, other, actualLen);
        mHeap[actualLen] = '\0';
        mLength = static_cast<uint32_t>(actualLen);
    } else {
        // 需要新分配堆内存
        char* newBuf = allocHeap(actualLen);
        memcpy(newBuf, other, actualLen);
        newBuf[actualLen] = '\0';

        if (!isLocal()) {
            SharedBuffer::bufferFromData(mHeap)->release();
        }

        mHeap = newBuf;
        mString = mHeap;
        mLength = static_cast<uint32_t>(actualLen);
        mCapacity = static_cast<uint32_t>(actualLen);
    }

    return static_cast<int32_t>(actualLen);
}

void String8::setTo(const String8& other)
{
    setTo(other.mString, other.mLength);
}

// ============== 赋值运算符 ==============

String8& String8::operator=(const String8& other)
{
    if (this != &other) {
        setTo(other. mString, other. mLength);
    }

    return *this;
}

String8& String8::operator=(const char* other)
{
    setTo(other);
    return *this;
}

String8& String8::operator=(String8&& other) noexcept
{
    if (this != &other) {
        release();

        if (other.isLocal()) {
            memcpy(mStack, other.mStack, other.mLength + 1);
            mString = mStack;
            mLength = other.mLength;
            mCapacity = LOCAL_STRING_SIZE - 1;
        } else {
            mHeap = other.mHeap;
            mString = mHeap;
            mLength = other.mLength;
            mCapacity = other. mCapacity;

            other.mString = other.mStack;
            other.mStack[0] = '\0';
            other.mLength = 0;
            other.mCapacity = LOCAL_STRING_SIZE - 1;
        }
    }
    return *this;
}

// ============== 字符串操作 ==============

String8 String8::left(uint32_t n) const
{
    if (n >= mLength) {
        return *this;
    }
    return String8(mString, n);
}

String8 String8::right(uint32_t n) const
{
    if (n >= mLength) {
        return *this;
    }
    return String8(mString + mLength - n, n);
}

void String8::trim(char c)
{
    if (empty()) {
        return;
    }

    ensureUnique();

    size_t start = 0;
    size_t end = mLength;

    while (start < end && mString[start] == c) {
        ++start;
    }
    while (end > start && mString[end - 1] == c) {
        --end;
    }

    if (start > 0) {
        memmove(mString, mString + start, end - start);
    }
    mLength = static_cast<uint32_t>(end - start);
    mString[mLength] = '\0';
}

void String8::trimLeft(char c)
{
    if (empty()) {
        return;
    }

    ensureUnique();

    size_t start = 0;
    while (start < mLength && mString[start] == c) {
        ++start;
    }

    if (start > 0) {
        mLength -= static_cast<uint32_t>(start);
        memmove(mString, mString + start, mLength + 1);
    }
}

void String8::trimRight(char c)
{
    if (empty()) {
        return;
    }

    ensureUnique();
    while (mLength > 0 && mString[mLength - 1] == c) {
        --mLength;
    }
    mString[mLength] = '\0';
}

String8 String8::reverse() const
{
    if (empty()) {
        return String8();
    }

    String8 ret(static_cast<uint32_t>(mLength));
    for (size_t i = 0; i < mLength; ++i) {
        ret. mString[i] = mString[mLength - 1 - i];
    }
    ret.mString[mLength] = '\0';
    ret.mLength = mLength;

    return ret;
}

String8 String8::substr(size_t start, size_t end) const
{
    if (start >= mLength || start > end) {
        return String8();
    }

    if (end >= mLength) {
        end = mLength - 1;
    }

    return String8(mString + start, end - start + 1);
}

bool String8::contains(const char* other) const
{
    if (other == nullptr) {
        return false;
    }

    return strstr(mString, other) != nullptr;
}

bool String8::removeAll(const char* other)
{
    if (other == nullptr || *other == '\0') {
        return false;
    }

    ensureUnique();

    bool removed = false;
    while (removeOne(other)) {
        removed = true;
    }
    return removed;
}

bool String8::removeOne(const char* other)
{
    int32_t pos = find(other);
    if (pos < 0) {
        return false;
    }

    size_t otherLen = strlen(other);
    size_t remaining = mLength - pos - otherLen;

    memmove(mString + pos, mString + pos + otherLen, remaining);
    mLength -= static_cast<uint32_t>(otherLen);
    mString[mLength] = '\0';

    return true;
}

int64_t String8::replaceAll(char o, char n)
{
    if (o == n || empty()) {
        return 0;
    }

    ensureUnique();

    int64_t count = 0;
    for (size_t i = 0; i < mLength; ++i) {
        if (mString[i] == o) {
            mString[i] = n;
            ++count;
        }
    }
    return count;
}

// ============== 查找操作 ==============

int32_t String8::find(const String8& other, size_t start) const
{
    return find(other.mString, start);
}

int32_t String8::find(const char* other, size_t start) const
{
    if (other == nullptr || start >= mLength) {
        return -1;
    }

    const char* result = strstr(mString + start, other);
    return result ? static_cast<int32_t>(result - mString) : -1;
}

int32_t String8::find(char c, size_t start) const
{
    if (start >= mLength) {
        return -1;
    }

    const char* result = strchr(mString + start, c);
    return result ? static_cast<int32_t>(result - mString) : -1;
}

void String8::findChar(int32_t& begin, int32_t& end, char c) const
{
    begin = -1;
    end = -1;

    for (size_t i = 0; i < mLength; ++i) {
        if (mString[i] == c) {
            if (begin < 0) begin = static_cast<int32_t>(i);
            end = static_cast<int32_t>(i);
        }
    }
}

void String8::findNotChar(int32_t& begin, int32_t& end, char c) const
{
    begin = -1;
    end = -1;

    for (size_t i = 0; i < mLength; ++i) {
        if (mString[i] != c) {
            if (begin < 0) begin = static_cast<int32_t>(i);
            end = static_cast<int32_t>(i);
        }
    }
}

int32_t String8::find_last_of(const char* key) const
{
    if (key == nullptr || *key == '\0' || empty()) {
        return -1;
    }

    size_t keyLen = strlen(key);
    if (keyLen > mLength) {
        return -1;
    }

    for (size_t i = mLength - keyLen; ; --i) {
        if (strncmp(mString + i, key, keyLen) == 0) {
            return static_cast<int32_t>(i);
        }
        if (i == 0) break;
    }

    return -1;
}

int32_t String8::find_last_of(const String8& str) const
{
    return find_last_of(str.c_str());
}

// ============== 比较操作 ==============

int32_t String8::stringcompare(const char* other) const
{
    if (mString == nullptr && other == nullptr) return 0;
    if (mString == nullptr) return -1;
    if (other == nullptr) return 1;
    return strcmp(mString, other);
}

int32_t String8::compare(const String8& other) const
{
    return stringcompare(other.mString);
}

int32_t String8::compare(const char* other) const
{
    return stringcompare(other);
}

int32_t String8::ncompare(const String8& other, size_t n) const
{
    return strncmp(mString, other.mString, n);
}

int32_t String8::ncompare(const char* other, size_t n) const
{
    if (other == nullptr) return 1;
    return strncmp(mString, other, n);
}

int32_t String8::casecmp(const String8& other) const
{
    return ::strcasecmp(mString, other.mString);
}

int32_t String8::casecmp(const char* other) const
{
    if (other == nullptr) return 1;
    return ::strcasecmp(mString, other);
}

// ============== 比较运算符 ==============

bool String8::operator<(const String8& other) const { return compare(other) < 0; }
bool String8::operator<=(const String8& other) const { return compare(other) <= 0; }
bool String8::operator==(const String8& other) const { return compare(other) == 0; }
bool String8::operator!=(const String8& other) const { return compare(other) != 0; }
bool String8::operator>=(const String8& other) const { return compare(other) >= 0; }
bool String8::operator>(const String8& other) const { return compare(other) > 0; }

bool String8::operator<(const char* other) const { return compare(other) < 0; }
bool String8::operator<=(const char* other) const { return compare(other) <= 0; }
bool String8::operator==(const char* other) const { return compare(other) == 0; }
bool String8::operator!=(const char* other) const { return compare(other) != 0; }
bool String8::operator>=(const char* other) const { return compare(other) >= 0; }
bool String8::operator>(const char* other) const { return compare(other) > 0; }

char& String8::operator[](size_t index)
{
    ensureUnique();
    if (index >= mCapacity) {
        return mString[mCapacity];
    }
    return mString[index];
}

const char& String8::operator[](size_t index) const
{
    if (index >= mLength) {
        return mString[mLength]; // 返回 '\0'
    }
    return mString[index];
}

// ============== 加法运算符 ==============

String8& String8::operator+=(const String8& other)
{
    append(other);
    return *this;
}

String8 String8::operator+(const String8& other) const
{
    String8 result(*this);
    result.append(other);
    return result;
}

String8& String8::operator+=(const char* other)
{
    append(other);
    return *this;
}

String8 String8::operator+(const char* other) const
{
    String8 result(*this);
    result.append(other);
    return result;
}

// ============== 大小写转换 ==============

void String8::toLower()
{
    toLower(0, SIZE_MAX);
}

void String8::toLower(size_t start, size_t numChars)
{
    if (start >= mLength) return;
    
    ensureUnique();
    
    size_t end = std::min(start + numChars, static_cast<size_t>(mLength));
    for (size_t i = start; i < end; ++i) {
        mString[i] = static_cast<char>(::tolower(static_cast<unsigned char>(mString[i])));
    }
}

void String8::toUpper()
{
    toUpper(0, SIZE_MAX);
}

void String8::toUpper(size_t start, size_t numChars)
{
    if (start >= mLength) return;
    
    ensureUnique();
    
    size_t end = std::min(start + numChars, static_cast<size_t>(mLength));
    for (size_t i = start; i < end; ++i) {
        mString[i] = static_cast<char>(::toupper(static_cast<unsigned char>(mString[i])));
    }
}

// ============== 格式化 ==============

String8 String8::Format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    String8 result = FormatV(fmt, args);
    va_end(args);
    return result;
}

String8 String8::FormatV(const char* fmt, va_list args)
{
    va_list tmp_args;
    va_copy(tmp_args, args);
    int32_t len = vsnprintf_(nullptr, 0, fmt, tmp_args);
    va_end(tmp_args);

    if (len <= 0) {
        return String8();
    }

    String8 result(static_cast<uint32_t>(len));
    vsnprintf_(result. mString, len + 1, fmt, args);
    result.mLength = static_cast<uint32_t>(len);

    return result;
}

int32_t String8::appendFormat(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int32_t result = appendFormatV(fmt, args);
    va_end(args);
    return result;
}

int32_t String8::appendFormatV(const char* fmt, va_list args)
{
    va_list tmp_args;
    va_copy(tmp_args, args);
    int32_t n = vsnprintf_(nullptr, 0, fmt, tmp_args);
    va_end(tmp_args);

    if (n <= 0) {
        return n;
    }

    size_t newLen = mLength + n;
    if (newLen >= MAX_STRING_SIZE) {
        return -1;
    }

    reserve(newLen);
    vsnprintf_(mString + mLength, n + 1, fmt, args);
    mLength = static_cast<uint32_t>(newLen);

    return n;
}

// ============== 静态方法 ==============

int32_t String8::GetNext(const String8& key, int32_t n)
{
    if (n < 2) return 0;
    if (n == 2) {
        return (key.mString[0] == key. mString[1]) ? 1 :  0;
    }

    int32_t max = 0;
    for (int32_t k = 1; k < n; ++k) {
        if (strncmp(key. c_str() + n - k, key. c_str(), k) == 0) {
            max = std::max(max, k);
        }
    }
    return max;
}

int32_t String8::KMP_strstr(const char* val, const char* key)
{
    if (val == nullptr || key == nullptr) {
        return -1;
    }

    int32_t valLen = static_cast<int32_t>(strlen(val));
    int32_t keyLen = static_cast<int32_t>(strlen(key));

    if (keyLen == 0) return 0;
    if (keyLen > valLen) return -1;

    for (int32_t i = 0; i <= valLen - keyLen; ) {
        int32_t j = 0;
        for (; j < keyLen; ++j) {
            if (val[i + j] != key[j]) {
                if (j > 0) {
                    i += (j - GetNext(String8(key, j), j));
                } else {
                    ++i;
                }
                break;
            }
        }
        if (j == keyLen) {
            return i;
        }
    }
    return -1;
}

size_t String8::Hash(const String8& obj)
{
#if defined(OS_WINDOWS)
    return std::_Hash_array_representation(obj.c_str(), obj.length());
#elif defined(OS_LINUX)
    return std::_Hash_impl::hash(obj.c_str(), obj.length());
#else
    // FNV-1a
    size_t hash = 14695981039346656037ULL;
    const char* data = obj.c_str();
    for (size_t i = 0; i < obj.length(); ++i) {
        hash ^= static_cast<size_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
#endif
}

// ============== 流输出 ==============

std::ostream& operator<<(std::ostream& out, const String8& in)
{
    out. write(in.c_str(), in.length());
    return out;
}

} // namespace eular