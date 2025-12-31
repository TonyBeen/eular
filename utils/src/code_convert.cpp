/*************************************************************************
    > File Name: code_convert.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年07月19日 星期五 16时44分28秒
 ************************************************************************/

#include "utils/code_convert.h"

#include <string.h>

#include <iconv.h>

#include "utils/exception.h"
#include "utils/errors.h"

#define CODE_UNSUPPORT "UNSUPPORT"

#define INVALID_ICONV_HANDLE ((iconv_t)-1)
#define INVALID_ICONV_RETURN ((size_t)-1)

#define CACHE_SIZE  1024

/**
 * UTF-8是一种多字节编码的字符集，表示一个Unicode字符时，它可以是1个至多个字节
 * 除了一字节外，左边1的个数表示当前编码字节数，utf8最多可以扩展到6个字节,中文字符占三个字节
 * 1字节：0xxxxxxx
 * 2字节：110xxxxx 10xxxxxx
 * 3字节：1110xxxx 10xxxxxx 10xxxxxx
 * 4字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
 */

#define IS_UTF8_ANSII(code) (0x00 < (uint8_t)(code) && (uint8_t)(code) < 0x80)  // ansii字符集
#define IS_UTF8_2BYTE(code) (0xc0 < (uint8_t)(code) && (uint8_t)(code) < 0xe0)  // 此范围内为2字节UTF-8字符
#define IS_UTF8_3BYTE(code) (0xe0 < (uint8_t)(code) && (uint8_t)(code) < 0xf0)  // 此范围内为3字节UTF-8字符
#define IS_UTF8_4BYTE(code) (0xf0 < (uint8_t)(code) && (uint8_t)(code) < 0xf8)  // 此范围内为4字节UTF-8字符
#define IS_UTF8_EXTRA(code) (0x80 < (uint8_t)(code) && (uint8_t)(code) < 0xc0)  // 其余的字符规则

namespace eular {
CodeConvert::CodeConvert() :
    m_codeConvHandle(INVALID_ICONV_HANDLE),
    m_cacheSize(0)
{
}

bool CodeConvert::convertBegin(CodeFlag from, CodeFlag to)
{
    iconv_t cd = iconv_open(_Flag2str(to), _Flag2str(from));
    if (cd == INVALID_ICONV_HANDLE) {
        perror("iconv_open");
        return false;
    }

    m_codeFrom = from;
    m_codeTo = to;
    m_codeConvHandle = cd;
    return true;
}

int32_t CodeConvert::convert(const std::string &from, std::string &to)
{
    if (from.empty()) {
        return STATUS(INVALID_PARAM);
    }

    char *pBegin = const_cast<char *>(from.c_str());
    size_t fromSize = from.size();
    std::string output;
    output.reserve(_ComputeOutSize(m_codeFrom, m_codeTo, from.size()));
    char outputBuf[CACHE_SIZE] = {0};

    int32_t result = STATUS(OK);
    do {
        char *pOutputBuf = outputBuf;
        size_t leftOutputLen = CACHE_SIZE;

        size_t nRet = iconv(m_codeConvHandle, &pBegin, &fromSize, &pOutputBuf, &leftOutputLen);
        if (INVALID_ICONV_RETURN == nRet) {
            switch (errno) {
            case EINVAL:
                // printf("An incomplete multibyte sequence has been encountered in the input.\n");
                result = -EINVAL;
                break;
            case EILSEQ:
                // printf("An invalid multibyte sequence has been encountered in the input.\n");
                result = -EILSEQ;
                break;
            case E2BIG:
                break;
            default:
                throw std::runtime_error("unknown error");
            }
        }

        if (result < 0) {
            output.clear();
            break;
        }

        // printf("nRet: %zu left: %zu leftOutputLen: %zu\n", nRet, fromSize, leftOutputLen);
        output.append(outputBuf, CACHE_SIZE - leftOutputLen);
    } while (fromSize > 0);

    to.append(output);
    return result;
}

void CodeConvert::convertEnd()
{
    if (m_codeConvHandle != INVALID_ICONV_HANDLE) {
        iconv_close(m_codeConvHandle);
    }
}

int32_t CodeConvert::UTF8ToGBK(const std::string &u8String, std::string &gbkString)
{
    if (u8String.empty()) {
        return STATUS(INVALID_PARAM);
    }

    iconv_t iconvHandle = iconv_open("GBK", "UTF-8");
    if (iconvHandle == INVALID_ICONV_HANDLE) {
        perror("iconv_open");
        return -errno;
    }

    char *pU8Begin = (char *)u8String.c_str();
    size_t inputSize = u8String.size();

    std::string gbkResult;
    gbkResult.reserve(_ComputeOutSize(UTF8, GBK, u8String.size()));

    int32_t result = STATUS(OK);
    do {
        char outputBuf[CACHE_SIZE] = {0};
        char *pOutputBuf = outputBuf;

        size_t outputLen = CACHE_SIZE;
        size_t leftOutputLen = CACHE_SIZE;

        size_t nRet = iconv(iconvHandle, &pU8Begin, &inputSize, &pOutputBuf, &leftOutputLen);
        if (INVALID_ICONV_RETURN == nRet) {
            switch (errno) {
            case EINVAL:
                // printf("An incomplete multibyte sequence has been encountered in the input.\n");
                result = -EINVAL;
                break;
            case EILSEQ:
                // printf("An invalid multibyte sequence has been encountered in the input.\n");
                result = -EILSEQ;
                break;
            case E2BIG:
                break;
            default:
                throw std::runtime_error("unknown error");
            }
        }

        if (result < 0) {
            gbkResult.clear();
            break;
        }

        // printf("nRet: %zu left: %zu leftOutputLen: %zu\n", nRet, inputSize, leftOutputLen);
        gbkResult.append(outputBuf, (outputLen - leftOutputLen));
    } while (inputSize > 0);

    gbkString.append(gbkResult);
    iconv_close(iconvHandle);
    return result;
}

int32_t CodeConvert::GBKToUTF8(const std::string &gbkString, std::string &u8String)
{
    if (gbkString.empty()) {
        return STATUS(INVALID_PARAM);
    }

    iconv_t iconvHandle = iconv_open("UTF-8", "GBK");
    if (iconvHandle == INVALID_ICONV_HANDLE) {
        perror("iconv_open");
        return -errno;
    }

    char *pBegin = (char *)gbkString.c_str();
    size_t inputSize = gbkString.size();

    std::string u8Result;
    u8Result.reserve(_ComputeOutSize(GBK, UTF8, gbkString.size()));

    int32_t result = STATUS(OK);
    do {
        char outputBuf[CACHE_SIZE] = {0};
        char *pOutputBuf = outputBuf;

        size_t outputLen = CACHE_SIZE;
        size_t leftOutputLen = CACHE_SIZE;

        size_t nRet = iconv(iconvHandle, &pBegin, &inputSize, &pOutputBuf, &leftOutputLen);
        if (INVALID_ICONV_RETURN == nRet) {
            switch (errno) {
            case EINVAL:
                // printf("An incomplete multibyte sequence has been encountered in the input.\n");
                result = -EINVAL;
                break;
            case EILSEQ:
                // printf("An invalid multibyte sequence has been encountered in the input.\n");
                result = -EILSEQ;
                break;
            case E2BIG:
                break;
            default:
                throw std::runtime_error("unknown error");
            }
        }

        if (result < 0) {
            u8Result.clear();
            break;
        }

        // printf("nRet: %zu left: %zu leftOutputLen: %zu\n", nRet, inputSize, leftOutputLen);
        u8Result.append(outputBuf, (outputLen - leftOutputLen));
    } while (inputSize > 0);

    u8String.append(u8Result);
    iconv_close(iconvHandle);
    return result;
}

int32_t CodeConvert::UTF8ToUTF16LE(const std::string &u8String, std::wstring &u16String)
{
    if (u8String.empty()) {
        return STATUS(INVALID_PARAM);
    }

    char *pU8Begin = (char *)u8String.c_str();
    size_t inputSize = u8String.size();

    std::wstring outU16String;
    outU16String.reserve(_ComputeOutSize(CodeFlag::UTF8, CodeFlag::UTF16LE, u8String.length()));

    iconv_t iconvHandle = iconv_open("UTF-16LE", "UTF-8");
    if (INVALID_ICONV_HANDLE == iconvHandle) {
        perror("iconv_open");
        return -errno;
    }

    int32_t result = STATUS(OK);
    while (inputSize > 0) {
        char outputBuf[CACHE_SIZE] = {0};

        char *pOutputBuf = outputBuf;
        size_t outputLen = CACHE_SIZE;
        size_t leftOutputLen = CACHE_SIZE;

        size_t nRet = iconv(iconvHandle, &pU8Begin, &inputSize, &pOutputBuf, &leftOutputLen);
        if (INVALID_ICONV_RETURN == nRet) {
            switch (errno) {
            case EINVAL:
                // printf("An incomplete multibyte sequence has been encountered in the input.\n");
                result = -EINVAL;
                break;
            case EILSEQ:
                // printf("An invalid multibyte sequence has been encountered in the input.\n");
                result = -EILSEQ;
                break;
            case E2BIG:
                break;
            default:
                throw std::runtime_error("unknown error");
            }
        }

        if (result < 0) {
            outU16String.clear();
            break;
        }

        outU16String.append((wchar_t *)outputBuf, (outputLen - leftOutputLen) / sizeof(wchar_t));
    }

    u16String.append(outU16String);
    iconv_close(iconvHandle);
    return result;
}

int32_t CodeConvert::UTF16LEToUTF8(const std::wstring &u16String, std::string &u8String)
{
    if (u16String.empty()) {
        return STATUS(INVALID_PARAM);
    }

    char *pU8Begin = (char *)u16String.c_str();
    size_t inputSize = u16String.size() * sizeof(wchar_t);

    std::string outU8String;
    outU8String.reserve(_ComputeOutSize(CodeFlag::UTF16LE, CodeFlag::UTF8, u16String.size()));

    iconv_t iconvHandle = iconv_open("UTF-8", "UTF-16LE");
    if (INVALID_ICONV_HANDLE == iconvHandle) {
        perror("iconv_open");
        return -errno;
    }

    int32_t result = STATUS(OK);
    while (inputSize > 0) {
        char outputBuf[CACHE_SIZE] = {0};

        char *pOutputBuf = outputBuf;
        size_t outputLen = CACHE_SIZE;
        size_t leftOutputLen = CACHE_SIZE;

        size_t nRet = iconv(iconvHandle, &pU8Begin, &inputSize, &pOutputBuf, &leftOutputLen);
        if (INVALID_ICONV_RETURN == nRet) {
            switch (errno) {
            case EINVAL:
                // printf("An incomplete multibyte sequence has been encountered in the input.\n");
                result = -EINVAL;
                break;
            case EILSEQ:
                // printf("An invalid multibyte sequence has been encountered in the input.\n");
                result = -EILSEQ;
                break;
            case E2BIG:
                break;
            default:
                throw std::runtime_error("unknown error");
            }
        }

        if (result < 0) {
            outU8String.clear();
            break;
        }

        outU8String.append(outputBuf, (outputLen - leftOutputLen));
    }

    u8String.append(outU8String);
    iconv_close(iconvHandle);
    return result;
}

int getChar(const char *str, size_t size)
{
    const char *start = str;
    const char *end = start + size;
    if (IS_UTF8_ANSII(*start)) {
        return 0;
    }

    if (IS_UTF8_2BYTE(*start)) {
        if ((start + 1) >= end) { // 当是utf8时，检测到2字节编码头时后面会跟着一个字符
            return -1;
        }
        if (IS_UTF8_EXTRA(*(start + 1))) {
            return 2;
        }
    }
    if (IS_UTF8_3BYTE(*start)) {
        if (start + 2 >= end) { // 检测到3字节编码头时，后面会有两个字节的位置
            return -1;
        }
        if (IS_UTF8_EXTRA(*(start + 1)) && IS_UTF8_EXTRA(*(start + 2))) {
            return 3;
        }
    }
    if (IS_UTF8_4BYTE(*start)) {
        if (start + 3 >= end) {
            return -1;
        }
        if (IS_UTF8_EXTRA(*(start + 1)) && IS_UTF8_EXTRA(*(start + 2)) && IS_UTF8_EXTRA(*(start + 3))) {
            return 4;
        }
    }

    return -1;
}

bool CodeConvert::IsUTF8Encode(const std::string & u8String, uint32_t *characterSize)
{
    if (u8String.empty()) {
        if (nullptr != characterSize) {
            *characterSize = 0;
        }
        return true;
    }

    uint32_t charSize = 0;
    const char *start = u8String.c_str();
    const char *end = start + u8String.length();
    bool isUtf8 = false;

    while (start < end) {
        if (IS_UTF8_ANSII(*start)) {
            ++start;
            ++charSize;
            continue;
        }

        if (IS_UTF8_2BYTE(*start)) {
            if ((start + 1) >= end) { // 当是utf8时，检测到2字节编码头时后面会跟着一个字符
                return false;
            }
            if (IS_UTF8_EXTRA(*(start + 1))) {
                isUtf8 = true;
            }
            start += 2;
            ++charSize;
            continue;
        }

        if (IS_UTF8_3BYTE(*start)) {
            if (start + 2 >= end) { // 检测到3字节编码头时，后面会有两个字节的位置
                return false;
            }
            if (IS_UTF8_EXTRA(*(start + 1)) && IS_UTF8_EXTRA(*(start + 2))) {
                isUtf8 = true;
            }
            start += 3;
            ++charSize;
            continue;
        }

        if (IS_UTF8_4BYTE(*start)) {
            if (start + 3 >= end) {
                return false;
            }
            if (IS_UTF8_EXTRA(*(start + 1)) && IS_UTF8_EXTRA(*(start + 2)) && IS_UTF8_EXTRA(*(start + 3))) {
                isUtf8 = true;
            }
            start += 4;
            ++charSize;
            continue;
        }

        isUtf8 = false;
        break;
    }

    if (isUtf8 && characterSize != nullptr) {
        *characterSize = charSize;
    }

    return isUtf8;
}

const char *CodeConvert::_Flag2str(CodeFlag flag)
{
    const char *str = CODE_UNSUPPORT;
    switch (flag) {
    case CodeFlag::GBK:
        str = "GBK";
        break;
    case CodeFlag::GB2312:
        str = "GB2312";
        break;
    case CodeFlag::UTF8:
        str = "UTF-8";
        break;
    case CodeFlag::UTF16LE:
        str = "UTF-16LE";
        break;
    case CodeFlag::UTF16BE:
        str = "UTF-16BE";
        break;
    default:
        break;
    }

    return str;
}

uint32_t CodeConvert::_ComputeOutSize(CodeFlag from, CodeFlag to, uint32_t inputSize)
{
    if (from == to) {
        return inputSize;
    }

    switch (to) {
    // 两个字节编码
    case CodeFlag::GBK:
    case CodeFlag::GB2312:
    case CodeFlag::UTF16LE:
    case CodeFlag::UTF16BE:
        return _ComputeOutSizeToGBK_UTF16(from, to, inputSize);
    case CodeFlag::UTF8:
        return _ComputeOutSizeToUTF8(from, to, inputSize);
    default:
        break;
    }

    return 0;
}

uint32_t CodeConvert::_ComputeOutSizeToGBK_UTF16(CodeFlag from, CodeFlag to, uint32_t inputSize)
{
    switch (from) {
    case CodeFlag::UTF8:
        return 2 * inputSize; // 按照最大size计算
    default:
        return inputSize;
    }

    return 0;
}

uint32_t CodeConvert::_ComputeOutSizeToUTF8(CodeFlag from, CodeFlag to, uint32_t inputSize)
{
    return 2 * inputSize; // 按照最大size计算, 4 * inputSize / 2
}

} // namespace eular
