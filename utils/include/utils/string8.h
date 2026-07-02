/*************************************************************************
    > File Name: string8.h
    > Author: hsz
    > Mail: utf-8(内部并不会监测是否utf-8, 只是表示此字符串为utf-8编码)
    > Created Time: 2021年04月11日 星期日 12时24分52秒
 ************************************************************************/

#ifndef __STRING8_H__
#define __STRING8_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <string>
#include <iostream>

#include <utils/sysdef.h>

#if COMPILER_TYPE == COMPILER_MSVC
    #include <sal.h>
    #define PRINTF_FMT _Printf_format_string_
    #define FORMAT_ATTR(...)
#else
    #define PRINTF_FMT
    #define FORMAT_ATTR(...) __attribute__((format(__VA_ARGS__)))
#endif

namespace eular {
class UTILS_API String8 {
public:
                        String8();
    explicit            String8(uint32_t size);
                        String8(const String8& other);
                        String8(const char* other);
                        String8(const char* other, size_t numChars);
                        String8(const std::string& other);
                        String8(String8 &&other) noexcept;
                        ~String8() noexcept;

    const char*         c_str() const noexcept;
    char *              data();
    String8             left(uint32_t n) const;
    String8             right(uint32_t n) const;
    void                trim(char c = ' ');
    void                trimLeft(char c = ' ');
    void                trimRight(char c = ' ');
    String8             reverse() const;
    void                reserve(size_t size);
    void                resize(size_t size);
    char&               front();
    const char&         front() const;
    char&               back();
    const char&         back() const;
    std::string         toStdString() const;

    bool                empty() const noexcept;
    size_t              length() const noexcept;
    size_t              capacity() const noexcept { return mCapacity; }
    void                clear();

    int32_t             append(char ch);
    int32_t             append(const String8& other);
    int32_t             append(const char* other);
    int32_t             append(const char* other, size_t numChars);

    static String8      Format(PRINTF_FMT const char* fmt, ...) FORMAT_ATTR(printf, 1, 2);
    int32_t             appendFormat(PRINTF_FMT const char* fmt, ...) FORMAT_ATTR(printf, 2, 3);

    String8&            operator=(const String8& other);
    String8&            operator=(const char* other);
    String8&            operator=(String8&& other) noexcept;

    String8&            operator+=(const String8& other);
    String8             operator+(const String8& other) const;
    String8&            operator+=(const char* other);
    String8             operator+(const char* other) const;

    int32_t             compare(const String8& other) const noexcept;
    int32_t             compare(const char* other) const noexcept;
    int32_t             ncompare(const String8& other, size_t n) const noexcept;
    int32_t             ncompare(const char* other, size_t n) const noexcept;

    int32_t             casecmp(const String8& other) const noexcept;
    int32_t             casecmp(const char* other) const noexcept;

    bool                operator<(const String8& other) const noexcept;
    bool                operator<=(const String8& other) const noexcept;
    bool                operator==(const String8& other) const noexcept;
    bool                operator!=(const String8& other) const noexcept;
    bool                operator>=(const String8& other) const noexcept;
    bool                operator>(const String8& other) const noexcept;

    bool                operator<(const char* other) const noexcept;
    bool                operator<=(const char* other) const noexcept;
    bool                operator==(const char* other) const noexcept;
    bool                operator!=(const char* other) const noexcept;
    bool                operator>=(const char* other) const noexcept;
    bool                operator>(const char* other) const noexcept;
    char&               operator[](size_t index);
    const char&         operator[](size_t index) const noexcept;

    // return the index of the first byte of other in this at or after
    // start, or -1 if not found
    int32_t             find(const String8 &other, size_t start = 0) const noexcept;
    int32_t             find(const char* other, size_t start = 0) const noexcept;
    int32_t             find(char c, size_t start = 0) const noexcept;
    void                findChar(int32_t &begin, int32_t &end, char c = ' ') const noexcept;
    void                findNotChar(int32_t &begin, int32_t &end, char c = ' ') const noexcept;
    int32_t             find_last_of(const char *str) const noexcept;
    int32_t             find_last_of(const String8 &str) const noexcept;

    String8             substr(size_t start, size_t end) const;
    bool                contains(const char* other) const;
    bool                removeAll(const char* other);
    int64_t             replaceAll(char o, char n);

    void                toLower();
    void                toLower(size_t start, size_t numChars);
    void                toUpper();
    void                toUpper(size_t start, size_t numChars);
    static size_t       Hash(const String8 &obj) noexcept;
    bool                isLocal() const noexcept { return mString == mStack; }

private:
    friend std::ostream&operator<<(std::ostream &out, const String8& in);
    char*               allocHeap(size_t numChars);
    void                release() noexcept;
    static String8      FormatV(const char* fmt, va_list args);
    int32_t             appendFormatV(const char* fmt, va_list args);
    void                setTo(const String8& other);
    int32_t             setTo(const char* other);
    int32_t             setTo(const char* other, size_t numChars);
    int32_t             stringcompare(const char *other) const noexcept;
    void                ensureUnique();
    bool                removeOne(const char *str);
    void                initFromChars(const char* str, size_t len);

private:
    enum {
        LOCAL_STRING_SIZE = 32,
    };
    union {
        char    mStack[LOCAL_STRING_SIZE];
        char*   mHeap;
    };

    char*       mString;
    uint32_t    mLength;
    uint32_t    mCapacity;
};

std::ostream& operator<<(std::ostream &out, const String8& in);

} // namespace eular

namespace std {
    template<>
    struct hash<eular::String8> {
        size_t operator()(const eular::String8 &obj) const noexcept {
            return eular::String8::Hash(obj);
        }
    };
}

#endif // __STRING8_H__
