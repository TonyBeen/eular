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
                        String8(const char* other, const size_t numChars);
                        String8(const std::string& other);
                        String8(String8 &&other);
                        ~String8();

    const char*         c_str() const;
    char *              data();
    String8             left(uint32_t n) const;         // 从左拷贝n个字符
    String8             right(uint32_t n) const;        // 从右拷贝n个字符
    void                trim(char c = ' ');             // 把字符串前后空格去掉
    void                trimLeft(char c = ' ');         // 去掉字符串左侧空格
    void                trimRight(char c = ' ');        // 去掉字符串右侧空格
    String8             reverse();                      // 翻转字符串
    void                reserve(size_t size);
    void                resize(size_t size);
    char&               front();
    const char&         front() const;
    char&               back();
    const char&         back() const;
    std::string         toStdString() const;            // String8 -> std::string

    bool                empty() const;
    size_t              length() const;
    size_t              capacity() const { return mCapacity; }
    void                clear();

    int                 append(char ch);
    int                 append(const String8& other);
    int                 append(const char* other);
    int                 append(const char* other, size_t numChars);

    static String8      Format(PRINTF_FMT const char* fmt, ...) FORMAT_ATTR(printf, 1, 2);
    int                 appendFormat(PRINTF_FMT const char* fmt, ...) FORMAT_ATTR(printf, 2, 3);

    String8&            operator=(const String8& other);
    String8&            operator=(const char* other);
    String8&            operator=(String8&& other);

    String8&            operator+=(const String8& other);
    String8             operator+(const String8& other) const;
    String8&            operator+=(const char* other);
    String8             operator+(const char* other) const;

    int                 compare(const String8& other) const;
    int                 compare(const char* other) const;
    int                 ncompare(const String8& other, size_t n) const;
    int                 ncompare(const char* other, size_t n) const;

    int                 casecmp(const String8& other) const;
    int                 casecmp(const char* other) const;

    bool                operator<(const String8& other) const;
    bool                operator<=(const String8& other) const;
    bool                operator==(const String8& other) const;
    bool                operator!=(const String8& other) const;
    bool                operator>=(const String8& other) const;
    bool                operator>(const String8& other) const;

    bool                operator<(const char* other) const;
    bool                operator<=(const char* other) const;
    bool                operator==(const char* other) const;
    bool                operator!=(const char* other) const;
    bool                operator>=(const char* other) const;
    bool                operator>(const char* other) const;
    char&               operator[](size_t index);
    const char&         operator[](size_t index) const;

    // return the index of the first byte of other in this at or after
    // start, or -1 if not found
    int32_t             find(const String8 &other, size_t start = 0) const;
    int32_t             find(const char* other, size_t start = 0) const;
    int32_t             find(const char c, size_t start = 0) const;
    // 找到第一个和最后一个c出现的位置，未找到则不更改begin和end值
    void                findChar(int &begin, int &end, char c = ' ') const;
    // 找到第一个不为c的字符和最后一个不为c的字符，如c = ' '，str = "  12345  ", begin = 2，end = 6
    void                findNotChar(int &begin, int &end, char c = ' ') const;
    // 返回最后一个字符串符合的位置,失败返回-1
    int32_t             find_last_of(const char *str) const;
    int32_t             find_last_of(const String8 &str) const;

    /**
     * @brief 复制从start到end的字符串
     * 
     * @param start 字符起始位置
     * @param end 字符结束位置
     * @return String8 
     */
    String8             substr(size_t start, size_t end) const;
    // return true if this string contains the specified substring
    bool                contains(const char* other) const;
    bool                removeAll(const char* other);
    int64_t             replaceAll(char o, char n);

    void                toLower();
    void                toLower(size_t start, size_t numChars);
    void                toUpper();
    void                toUpper(size_t start, size_t numChars);
    // 未匹配到或参数错误返回负值，否则返回匹配到的字符串位置
    static int32_t      KMP_strstr(const char *val, const char *key);
    static size_t       Hash(const String8 &obj);

private:
    friend std::ostream&operator<<(std::ostream &out, const String8& in);
    char*               getBuffer(size_t numChars = 0);
    void                release();
    static String8      FormatV(const char* fmt, va_list args);
    int                 appendFormatV(const char* fmt, va_list args);
    void                setTo(const String8& other);
    int                 setTo(const char* other);
    int                 setTo(const char* other, size_t numChars);
    int                 stringcompare(const char *other) const;
    // kmp next数组获取，非-1版本
    static int          GetNext(String8 key, int n);
    void                detach();
    bool                removeOne(const char *str);

private:
    char *      mString;
    uint32_t    mCapacity; // 为实际内存大小减1
};

// 为了方便输出
std::ostream&   operator<<(std::ostream &out, const String8& in);

} // namespace eular

namespace std {
    template<>
    struct hash<eular::String8> {
        size_t operator()(const eular::String8 &obj) const {
            return eular::String8::Hash(obj);
        }
    };
}

#endif // __STRING8_H__
