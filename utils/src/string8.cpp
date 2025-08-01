/*************************************************************************
    > File Name: string8.cpp
    > Author: hsz
    > Mail:
    > Created Time: 2021年04月11日 星期日 12时25分02秒
 ************************************************************************/

#include "utils/string8.h"

#include <assert.h>

#include "utils/utils.h"
#include "utils/shared_buffer.h"
#include "utils/debug.h"
#include "utils/errors.h"
#include "utils/exception.h"

#define DEFAULT_STRING_SIZE 64
#define MAXSIZE (1024 * 1024) // 1Mb

namespace eular {

static inline char* getEmptyString()
{
    static SharedBuffer* gEmptyStringBuf = [] {
        SharedBuffer* buf = SharedBuffer::alloc(1);
        char* str = static_cast<char*>(buf->data());
        *str = 0;
        return buf;
    }();

    ::atexit([]() {
        gEmptyStringBuf->release();
    });

    gEmptyStringBuf->acquire();
    return static_cast<char*>(gEmptyStringBuf->data());
}

static char* allocFromUTF8(const char* in, size_t len)
{
    if (in && len > 0 && len < MAXSIZE) {
        SharedBuffer* buf = SharedBuffer::alloc(len + 1);
        assert(buf && "Unable to allocate shared buffer");
        if (buf) {
            char* str = static_cast<char *>(buf->data());
            memset(str, 0, len + 1); // 防止因 strlen(in) < len 而导致\0的位置出现偏差
            memcpy(str, in, len);
            return str;
        }
    }

    return nullptr;
}

char *String8::getBuffer(size_t numChars)
{
    if (numChars < DEFAULT_STRING_SIZE) {   // 小于默认字符串长度
        mCapacity = DEFAULT_STRING_SIZE;
    } else if (numChars < MAXSIZE) {        // 小于最大长度则就申请(numChars + 1)
        mCapacity = numChars;
    } else {                                // 大于最大长度，抛出异常
        throw Exception("String8::getBuffer() too many characters");
    }

    SharedBuffer *psb = SharedBuffer::alloc(mCapacity + 1);
    if (psb == nullptr) {
        throw Exception("Unable to allocate shared buffer");
    }
    char *buf = static_cast<char *>(psb->data());
    memset(buf, 0, mCapacity + 1);
    return buf;
}

/**
 * @brief 将当前String8从共享中分离，如果只有当前String8使用则无操作
 * 
 */
void String8::detach()
{
    SharedBuffer *psb = SharedBuffer::bufferFromData(mString);
    if (psb == nullptr) {
        mString = getEmptyString();
        return;
    }

    if (psb->onlyOwner() == false) {
        SharedBuffer *new_psb = psb->editResize(mCapacity + 1);
        mString = static_cast<char *>(new_psb->data());
    }
}

void String8::release()
{
    if (mString) {
        SharedBuffer::bufferFromData(mString)->release();
    }

    mString = nullptr;
    mCapacity = 0;
}

String8::String8() :
    mString(getEmptyString()),
    mCapacity(0)
{
}

String8::String8(uint32_t size) :
    mString(getBuffer(size))
{
}

String8::String8(const String8& other) :
    mString(other.mString),
    mCapacity(other.mCapacity)
{
    if (mString == nullptr) {
        throw Exception("invalid String8");
    }

    SharedBuffer::bufferFromData(mString)->acquire();
}

String8::String8(const char* other) :
    mString(allocFromUTF8(other, strlen(other))),
    mCapacity(strlen(other))
{
    if (mString == nullptr) {
        mString = getEmptyString();
        mCapacity = 0;
    }
}

String8::String8(const char* other, const size_t numChars) :
    mString(allocFromUTF8(other, numChars)),
    mCapacity(numChars)
{
    if (mString == nullptr) {
        mString = getEmptyString();
        mCapacity = 0;
    }
}

String8::String8(const std::string& other) :
    mString(allocFromUTF8(other.c_str(), other.length())),
    mCapacity(other.length())
{
    if (mString == nullptr) {
        mString = getEmptyString();
        mCapacity = 0;
    }
}

String8::String8(String8 &&other) :
    mString(getEmptyString()),
    mCapacity(0)
{
    assert(other.mString != nullptr);
    std::swap(mString, other.mString);
    std::swap(mCapacity, other.mCapacity);
}

String8::~String8()
{
    release();
}

int32_t String8::find(const String8 &other, size_t start) const
{
    assert(mString && other.mString);
    const char *tmp = strstr(mString + start, other.mString);
    return tmp ? (tmp - mString) : -1;
}

int32_t String8::find(const char* other, size_t start) const
{
    assert(mString);
    const char *tmp = strstr(mString + start, other);
    return tmp ? (tmp - mString) : -1;
}

int32_t String8::find(const char c, size_t start) const
{
    assert(mString);
    const char *index = strchr(mString + start, c);
    return index ? (index - mString) : -1;
}

const char* String8::c_str() const
{
    return mString;
}

char *String8::data()
{
    return mString;
}

String8 String8::left(uint32_t n) const
{
    String8 ret(mString, n);
    return ret;
}

String8 String8::right(uint32_t n) const
{
    if (length() <= n) {
        return String8();
    }

    size_t offset = length() - n;
    return std::move(String8(mString + offset, n));
}

void String8::trim(char c)
{
    detach();
    int begin = -1;
    int end = -1;
    findNotChar(begin, end, c);

    // 全是c字符
    if (begin < 0 && end < 0) {
        mString[0] = '\0';
        return;
    }

    // 全不为c
    if (0 == begin && (size_t)end == length()) {
        return;
    }

    memmove(mString, mString + begin, end - begin + 1);
    mString[end - begin + 1] = '\0';
}

void String8::trimLeft(char c)
{
    detach();
    int begin = -1;
    int end = -1;
    findNotChar(begin, end, c);
    // 全是c字符
    if (begin < 0 && end < 0) {
        mString[0] = '\0';
        return;
    }

    // 左侧无c字符
    if (begin == 0) {
        return;
    }

    size_t len = length();
    memmove(mString, mString + begin, len - begin);
    mString[len - begin] = '\0';
}

void String8::trimRight(char c)
{
    detach();
    int begin = -1;
    int end = -1;
    findNotChar(begin, end, c);

    // 全是c字符
    if (begin < 0 && end < 0) {
        mString[0] = '\0';
        return;
    }

    size_t len = length();
    // 右侧无c字符
    if ((size_t)end == (len - 1)) {
        return;
    }
    mString[end + 1] = '\0';
}

String8 String8::reverse()
{
    if (length() == 0) {
        return String8();
    }

    String8 ret = this->c_str();
    ret.clear();
    char *buf = ret.data();
    if (buf == nullptr) {
        return ret;
    }
    for (size_t i = 0; i < length(); ++i) {
        buf[i] = mString[length() - 1 - i];
    }

    return ret;
}

void String8::reserve(size_t size)
{
    detach();
    if (mString == nullptr) {
        mString = getBuffer(size);
    }

    if (mCapacity < size) {
        release();
        mString = getBuffer(size);
    }
}

void String8::resize(size_t size)
{
    String8 temp(mString, size);
    *this = std::move(temp);
}

char &String8::front()
{
    if (empty()) {
        throw Exception("length == 0");
    }
    return mString[0];
}

const char &String8::front() const
{
    if (empty()) {
        throw Exception("length == 0");
    }

    return mString[0];
}

char &String8::back()
{
    if (empty()) {
        throw Exception("length == 0");
    }

    size_t size = length();
    return mString[size - 1];
}

const char &String8::back() const
{
    if (empty()) {
        throw Exception("length == 0");
    }

    size_t size = length();
    return mString[size - 1];
}

std::string String8::toStdString() const
{
    return std::string(mString);
}

bool String8::empty() const
{
    return mString[0] == '\0';
}

size_t String8::length() const
{
    size_t len = 0;
    if (mString) {
        len = strlen(mString);
    }

    return len;
}

void String8::clear()
{
    detach();
    if (mString) {
        memset(mString, 0, strlen(mString));
    }
}

int String8::append(char ch)
{
    char arrayTemp[2] = {0};
    arrayTemp[0] = ch;

    return append(arrayTemp, 1);
}

int String8::append(const String8 &other)
{
    return append(other.mString, other.length());
}

int String8::append(const char* other)
{
    return append(other, strlen(other));
}

int String8::append(const char* other, size_t numChars)
{
    if (other == nullptr) {
        return 0;
    }

    if (numChars == 0) {
        return 0;
    } 

    if (length() == 0) {
        return setTo(other, numChars);
    }

    size_t size = strlen(other);
    size = (size > numChars) ? numChars : size;
    if (size > numChars) {
        size = numChars;
    }
    if (size == 0) {
        return 0;
    }

    size_t oldLen = length();
    size_t totalSize = size + oldLen;
    if (totalSize < mCapacity) {
        detach();
        memmove(mString + oldLen, other, size);
        mString[totalSize] = '\0';
        return size;
    } else {
        char *buf = getBuffer(totalSize);
        if (mString) {
            memcpy(buf, mString, length());
        }
        memcpy(buf + length(), other, size);
        size_t cap = mCapacity;
        this->release();
        mString = buf;
        mCapacity = cap;
        return size;
    }

    return 0;
}

String8& String8::operator=(const String8& other)
{
    if (&other != this) {
        release();
        mString = other.mString;
        mCapacity = other.mCapacity;
        SharedBuffer::bufferFromData(mString)->acquire();
    }

    return *this;
}

String8& String8::operator=(const char* other)
{
    setTo(other);
    return *this;
}

String8& String8::operator=(String8&& other)
{
    if (&other != this) {
        std::swap(mString, other.mString);
        std::swap(mCapacity, other.mCapacity);
    }

    return *this;
}

String8& String8::operator+=(const String8& other)
{
    this->append(other);
    return *this;
}
String8 String8::operator+(const String8& other) const
{
    String8 tmp(*this);
    tmp += other;
    return tmp;
}
String8& String8::operator+=(const char* other)
{
    this->append(other);
    return *this;
}
String8 String8::operator+(const char* other) const
{
    String8 tmp(*this);
    tmp += other;
    return tmp;
}

int String8::compare(const String8& other) const
{
    return stringcompare(other.mString);
}

int String8::compare(const char* other) const
{
    return stringcompare(other);
}

int String8::ncompare(const String8& other, size_t n) const
{
    return strncmp(mString, other.mString, n);
}

int String8::ncompare(const char* other, size_t n) const
{
    if (other == nullptr) {
        return 1;
    }
    return strncmp(mString, other, n);
}

int String8::strcasecmp(const String8& other) const
{
    if (mString) {
        return ::strcasecmp(mString, other.mString);
    }

    return -EPERM;
}

int String8::strcasecmp(const char* other) const
{
    if (mString) {
        return ::strcasecmp(mString, other);
    }
    return -EPERM;
}

bool String8::operator<(const String8& other) const
{
    return stringcompare(other.mString) < 0;
}
bool String8::operator<=(const String8& other) const
{
    return stringcompare(other.mString) <= 0;
}
bool String8::operator==(const String8& other) const
{
    return stringcompare(other.mString) == 0;
}
bool String8::operator!=(const String8& other) const
{
    return stringcompare(other.mString) != 0;
}
bool String8::operator>=(const String8& other) const
{
    return stringcompare(other.mString) >= 0;
}
bool String8::operator>(const String8& other) const
{
    return stringcompare(other.mString) > 0;
}

bool String8::operator<(const char* other) const
{
    return stringcompare(other) < 0;
}
bool String8::operator<=(const char* other) const
{
    return stringcompare(other) <= 0;
}
bool String8::operator==(const char* other) const
{
    return stringcompare(other) == 0;
}
bool String8::operator!=(const char* other) const
{
    return stringcompare(other) != 0;
}
bool String8::operator>=(const char* other) const
{
    return stringcompare(other) >= 0;
}
bool String8::operator>(const char* other) const
{
    return stringcompare(other) > 0;
}

// 如果index超过范围则返回最后一个位置'\0'
char& String8::operator[](size_t index)
{
    detach();
    if (mString == nullptr) {
        mString = getBuffer(index);
        if (mString == nullptr) {
            throw Exception("no memory");
        }
    }
    if (index >= mCapacity) {
        return mString[mCapacity];
    }
    return mString[index];
}

const char& String8::operator[](size_t index) const
{
    if (mString == nullptr) {
        throw Exception("no memory");
    }
    if (index >= mCapacity) {
        return mString[mCapacity];
    }
    return mString[index];
}

int String8::stringcompare(const char *other) const
{
    if (!mString || !other) {
        return -0xFFFF;         // means param error
    }
    return strcmp(mString, other);
}

int String8::GetNext(String8 key, int n)
{
    if (n < 2) {
        return 0;
    }
    if (n == 2) {
        if (key[0] == key[1]) {
            return 1;
        }
        return 0;
    }
    int max = 0;
    for (int k = 1; k < n; ++k) {
        if (strncmp(key.c_str() + n - k, key.c_str(), k) == 0) {
            max = k > max ? k : max;
        }
    }
    return max;
}

void String8::setTo(const String8& other)
{
    setTo(other.mString, other.length());
}

int String8::setTo(const char* other)
{
    size_t len = strlen(other);
    return setTo(other, len);
}

int String8::setTo(const char* other, size_t numChars)
{
    if (other == nullptr || numChars == 0) {
        return STATUS(INVALID_PARAM);
    }
    int ret = 0;

    size_t otherLen = strlen(other);
    numChars = numChars > otherLen ? otherLen : numChars;

    if (mString == nullptr) {
        mString = getBuffer(numChars);
        assert(mString);
        memcpy(mString, other, numChars);
        return numChars;
    }

    if (mCapacity < numChars) {
        release();
        mString = getBuffer(numChars);
        if (mString) {
            memmove(mString, other, numChars);
            ret = numChars;
        }
    } else {
        if (SharedBuffer::bufferFromData(mString)->onlyOwner()) {
            clear();
            if (mString) {
                memmove(mString, other, numChars);
                ret = numChars;
            }
        } else {
            String8 temp(other, numChars);
            std::swap(mString, temp.mString);
        }
    }

    return ret;
}

void String8::findNotChar(int &begin, int &end, char c) const
{
    const int len = length();
    for (int i = 0; i < len; ++i) {
        if (mString[i] == c) {
            continue;
        }
        begin = i;
        break;
    }
    for (int i = len - 1; i > 0; --i) {
        if (mString[i] == c) {
            continue;
        }
        end = i;
        break;
    }
}

int32_t String8::find_last_of(const char *key) const
{
    // 思想，将两个字符串均翻转后在匹配，在使用字符串长度减去匹配字符串长度减查找到的字符串位置
    String8 keyStr = key;
    String8 value = mString;
    String8 valReverse = value.reverse();
    String8 keyReverse = keyStr.reverse();
    assert(valReverse.length() == value.length() && keyReverse.length() == keyStr.length());
    int index = valReverse.find(keyReverse.c_str());
    if (index < 0) {
        return index;
    }
    return length() - index - keyStr.length();
}

int32_t String8::find_last_of(const String8 &str) const
{
    return find_last_of(str.c_str());
}

String8 String8::substr(size_t start, size_t end)
{
    String8 ret;
    if (start >= length() || end >= length() || start >= end) {
        return ret;
    }

    ret = String8(mString + start, end);
    return ret;
}

bool String8::contains(const char* other) const
{
    return nullptr != strstr(mString, other);
}

bool String8::removeAll(const char* other)
{
    detach();
    while (removeOne(other));
    return true;
}

// Remove the first matching string found
bool String8::removeOne(const char *other)
{
    // detach();
    int start = find(other);
    if (start < 0) {
        return false;
    }

    size_t oldLen = length();
    size_t otherLen = strlen(other);
    size_t end = otherLen + start;
    memmove(mString + start, mString + end, length() - end);
    mString[oldLen - otherLen] = '\0';
    return true;
}

/**
 * @brief 将所有的o字符替换为n字符
 * 
 * @param o old char
 * @param n new char
 * @return int64_t 
 */
int64_t String8::replaceAll(char o, char n)
{
    if (n == o) {
        return 0;
    }

    int64_t count = 0;
    for (size_t i = 0; i < length(); ++i) {
        if (mString[i] == o) {
            mString[i] = n;
            ++count;
        }
    }

    return count;
}

void String8::toLower()
{
    toLower(0, SIZE_MAX);
}

void String8::toLower(size_t start, size_t numChars)
{
    size_t size = length();
    if (size > start) {
        for (size_t i = start; i < numChars && i < size; ++i) { // i < size -> anti overflow
            mString[i] = static_cast<char>(::tolower(mString[i]));
        }
    }
}

void String8::toUpper()
{
    toUpper(0, SIZE_MAX);
}

void String8::toUpper(size_t start, size_t numChars)
{
    size_t size = length();
    if (size > start) {
        for (size_t i = start; i < numChars && i < size; ++i) { // i < size -> anti overflow
            mString[i] = static_cast<char>(::toupper(mString[i]));
        }
    }
}

int32_t String8::KMP_strstr(const char *val, const char *key)
{
    if (val == nullptr || key == nullptr) {
        return STATUS(INVALID_PARAM);
    }
    int valLen = strlen(val);
    int keyLen = strlen(key);
    int i = 0;
    int j = 0;
    for (i = 0; i < valLen;) {
        for (j = 0; j < keyLen; ++j) {
            if (val[i + j] == key[j]) {
                continue;
            }
            // j > 0: 表示当前存在匹配上的一段字符串，但是不完全匹配，所以需要偏移
            if (j > 0) {
                i += (j - GetNext(String8(key, j), j));
            } else { // 没有匹配到一个字符则只移动一个位置
                ++i;
            }
            break;
        }
        if (j == keyLen) { // 由continue跳出循环时需判断是否j == 子串长度
            return i;
        }
    }
    return -1;
}

size_t String8::Hash(const String8 &obj)
{
    return std::_Hash_impl::hash(obj.c_str(), obj.length());
}

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
    String8 result;
    int len = 0;
    char *buf = nullptr;

    va_list tmp_args;
    va_copy(tmp_args, args);
    len = vsnprintf(nullptr, 0, fmt, tmp_args);
    va_end(tmp_args);

    if (len > 0) {
        size_t cap = len;
        SharedBuffer *psb = SharedBuffer::alloc(cap + 1);
        if (psb == nullptr) {
            return String8();
        }
        buf = static_cast<char *>(psb->data());
        vsnprintf(buf, cap + 1, fmt, args);
        buf[cap] = '\0';
        result.release();
        result.mString = buf;
        result.mCapacity = cap;
    }

    return result;
}

int String8::appendFormat(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    int numChars = appendFormatV(fmt, args);

    va_end(args);
    return numChars;
}

int String8::appendFormatV(const char* fmt, va_list args)
{
    int n = 0;
    va_list tmp_args;
    va_copy(tmp_args, args);
    n = vsnprintf(nullptr, 0, fmt, tmp_args);
    va_end(tmp_args);
    if (n <= 0) {
        return n;
    }

    size_t oldLength = length();
    if ((oldLength + n) > MAXSIZE - 1) {
        return -1;
    }

    detach();

    SharedBuffer *oldBuffer = SharedBuffer::bufferFromData(mString);
    UNUSED(oldBuffer);
    char *buf = nullptr;
    if (mCapacity < (oldLength + n)) {
        SharedBuffer *buffer = SharedBuffer::bufferFromData(mString)->editResize(oldLength + n + 1);
        if (buffer == nullptr) {
            return -1;
        }
        buf = static_cast<char *>(buffer->data());
        mCapacity = oldLength + n;
    }

    if (buf) {
        vsnprintf(buf + oldLength, n + 1, fmt, args);
        mString = buf;
        return n;
    }

    vsnprintf(mString + oldLength, n + 1, fmt, args);
    return n;
}

std::ostream& operator<<(std::ostream &out, const String8& in)
{
    out << in.c_str();
    return out;
}

} // namespace eular
